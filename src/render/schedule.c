#include "render/schedule.h"

#include <stddef.h>
#include <string.h>

#define START_ESTIMATE_NS 1000000LL // 1ms until real samples say otherwise
#define FALLBACK_PERIOD_NS 16666667LL // a mode without a refresh rate (headless)

static const char *const names[] = {
	[SCHEDULE_ADAPTIVE] = "adaptive",
	[SCHEDULE_IMMEDIATE] = "immediate",
	[SCHEDULE_DEADLINE] = "deadline",
};

bool schedule_policy_parse(const char *value, enum schedule_policy *policy) {
	if (!value) {
		*policy = SCHEDULE_ADAPTIVE;
		return true;
	}
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (!strcmp(value, names[i])) {
			*policy = (enum schedule_policy)i;
			return true;
		}
	}
	return false;
}

const char *schedule_policy_name(enum schedule_policy policy) {
	return names[policy];
}

static void reset_budget(struct schedule *schedule) {
	schedule->cpu = START_ESTIMATE_NS;
	schedule->gpu = START_ESTIMATE_NS;
	schedule->overlap = false;
}

void schedule_init(struct schedule *schedule, enum schedule_policy policy, int64_t period) {
	memset(schedule, 0, sizeof(*schedule));
	schedule->policy = policy;
	schedule->active = true;
	// startup wants a first frame
	schedule->redraw = true;
	schedule_set_period(schedule, period);
	reset_budget(schedule);
}

void schedule_set_period(struct schedule *schedule, int64_t period) {
	schedule->period = period > 0 ? period : FALLBACK_PERIOD_NS;
	schedule->have_vblank = false;
}

void schedule_request_redraw(struct schedule *schedule) {
	schedule->redraw = true;
}

int64_t schedule_lead(const struct schedule *schedule) {
	int64_t lead = schedule->cpu + schedule->gpu + SCHEDULE_MARGIN_NS;
	return lead < schedule->period ? lead : schedule->period;
}

bool schedule_overlapping(const struct schedule *schedule) {
	return schedule->policy == SCHEDULE_DEADLINE
		|| (schedule->policy == SCHEDULE_ADAPTIVE && schedule->overlap);
}

int64_t schedule_next_vblank(const struct schedule *schedule, int64_t t) {
	int64_t period = schedule->period;
	if (!schedule->have_vblank) {
		return t + period; // no grid yet, a refresh from now
	}
	int64_t diff = t - schedule->vblank;
	int64_t steps = diff >= 0 ? diff / period + 1 : -((-diff) / period);
	int64_t next = schedule->vblank + steps * period;
	return next > t ? next : next + period;
}

// tied to the pending submission, not to now: a missed window stays open
// instead of chasing the next refresh
static int64_t render_window(const struct schedule *schedule) {
	return schedule_next_vblank(schedule, schedule->pending_submit) - schedule_lead(schedule);
}

bool schedule_render_due(const struct schedule *schedule, int64_t now) {
	if (!schedule->active || !schedule->redraw || schedule->queued) {
		return false;
	}
	if (!schedule->pending) {
		return true;
	}
	return schedule_overlapping(schedule) && now >= render_window(schedule);
}

static bool wake_expired(const struct schedule *schedule, int64_t now) {
	return schedule->callbacks_wake && now >= schedule->callbacks_wake;
}

// a real handoff to KMS: everyone gets a new turn
static void new_cycle(struct schedule *schedule) {
	schedule->cycle = SCHEDULE_CYCLE_NEW;
	schedule->callbacks_wake = 0;
}

// a flip or an empty pass: catch whoever hasn't had a turn. never downgrades
// a new cycle that's already due
static void current_cycle(struct schedule *schedule) {
	if (schedule->cycle == SCHEDULE_CYCLE_NONE) {
		schedule->cycle = SCHEDULE_CYCLE_CURRENT;
	}
}

void schedule_submitted(struct schedule *schedule, void *ticket, int64_t now) {
	schedule->pending = ticket;
	schedule->pending_submit = now;
	schedule->redraw = false;
	new_cycle(schedule);
}

void schedule_queued(struct schedule *schedule, void *ticket) {
	// a successor doesn't wake clients, its promotion does
	schedule->queued = ticket;
	schedule->redraw = false;
}

void schedule_empty(struct schedule *schedule, int64_t now) {
	schedule->redraw = false;
	// the grid moved on since the last wake: that's a new cycle (rendering and
	// an expired wake can land in the same dispatch)
	if (wake_expired(schedule, now)) {
		schedule->cycle = SCHEDULE_CYCLE_NEW;
	}
	// the next wake on the grid, nothing gets flipped for it
	schedule->callbacks_wake = schedule_next_vblank(schedule, now);
	current_cycle(schedule);
}

void *schedule_presented(struct schedule *schedule, int64_t kernel, int64_t now) {
	if (!schedule->pending) {
		return NULL; // nobody waited for this one, the clock stays
	}
	int64_t anchor = kernel > 0 ? kernel : now;
	// a late or out of order timestamp never drags the grid backwards
	if (!schedule->have_vblank || anchor > schedule->vblank) {
		schedule->vblank = anchor;
		schedule->have_vblank = true;
	}
	void *ticket = schedule->pending;
	schedule->pending = NULL;
	if (!schedule->queued) {
		current_cycle(schedule);
	}
	return ticket;
}

void schedule_promoted(struct schedule *schedule, int64_t submit) {
	schedule->pending = schedule->queued;
	schedule->queued = NULL;
	// its own watchdog, starting from the real handoff
	schedule->pending_submit = submit;
	// preparing it didn't wake anyone, reaching KMS does
	new_cycle(schedule);
}

void *schedule_discard_queued(struct schedule *schedule) {
	void *ticket = schedule->queued;
	schedule->queued = NULL;
	schedule->redraw = true;
	return ticket;
}

// bigger samples count at once, smaller ones close 1/16 of the gap. each
// capped at a refresh so one awful frame can't poison it for long
static void estimate(int64_t *estimate, int64_t sample, int64_t period) {
	if (sample < 0) {
		return;
	}
	if (sample > period) {
		sample = period;
	}
	if (sample >= *estimate) {
		*estimate = sample;
	} else {
		*estimate -= (*estimate - sample) / 16;
	}
}

// hysteresis: on at 3/4 of a refresh, off at 1/2, so small wobbles don't flip it
static void update_overlap(struct schedule *schedule) {
	if (schedule->policy != SCHEDULE_ADAPTIVE) {
		return;
	}
	int64_t lead = schedule_lead(schedule);
	if (!schedule->overlap && lead * 4 >= schedule->period * 3) {
		schedule->overlap = true;
	} else if (schedule->overlap && lead * 2 <= schedule->period) {
		schedule->overlap = false;
	}
}

void schedule_observe_cpu(struct schedule *schedule, int64_t ns) {
	estimate(&schedule->cpu, ns, schedule->period);
	update_overlap(schedule);
}

void schedule_observe_gpu(struct schedule *schedule, int64_t ns) {
	estimate(&schedule->gpu, ns, schedule->period);
	update_overlap(schedule);
}

bool schedule_callbacks_due(const struct schedule *schedule, int64_t now) {
	return schedule->active
		&& (schedule->cycle != SCHEDULE_CYCLE_NONE || wake_expired(schedule, now));
}

bool schedule_callbacks_new_cycle(const struct schedule *schedule, int64_t now) {
	return schedule->cycle == SCHEDULE_CYCLE_NEW || wake_expired(schedule, now);
}

void schedule_callbacks_sent(struct schedule *schedule, int64_t now) {
	schedule->cycle = SCHEDULE_CYCLE_NONE;
	if (wake_expired(schedule, now)) {
		schedule->callbacks_wake = 0;
	}
}

static void earliest(int64_t *best, int64_t candidate) {
	if (candidate > 0 && (*best == 0 || candidate < *best)) {
		*best = candidate;
	}
}

int64_t schedule_deadline(const struct schedule *schedule, int64_t now) {
	if (!schedule->active) {
		return 0;
	}
	int64_t best = 0;
	if (schedule->redraw && !schedule->queued) {
		if (!schedule->pending) {
			earliest(&best, now);
		} else if (schedule_overlapping(schedule)) {
			earliest(&best, render_window(schedule));
		}
	}
	if (schedule->cycle != SCHEDULE_CYCLE_NONE) {
		earliest(&best, now);
	}
	earliest(&best, schedule->callbacks_wake);
	if (schedule->pending) {
		earliest(&best, schedule->pending_submit + SCHEDULE_WATCHDOG_NS);
	}
	return best;
}

bool schedule_stalled(const struct schedule *schedule, int64_t now) {
	return schedule->pending && now - schedule->pending_submit > SCHEDULE_WATCHDOG_NS;
}

void schedule_pause(struct schedule *schedule, void **pending, void **queued) {
	*pending = schedule->pending;
	*queued = schedule->queued;
	schedule->pending = NULL;
	schedule->queued = NULL;
	schedule->redraw = false;
	schedule->cycle = SCHEDULE_CYCLE_NONE;
	schedule->callbacks_wake = 0;
	schedule->active = false;
}

void schedule_resume(struct schedule *schedule) {
	schedule->active = true;
	schedule->have_vblank = false;
	schedule->redraw = true;
	schedule->cycle = SCHEDULE_CYCLE_NONE;
	schedule->callbacks_wake = 0;
	reset_budget(schedule);
}
