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

void schedule_submitted(struct schedule *schedule, void *ticket, int64_t now) {
	schedule->pending = ticket;
	schedule->pending_submit = now;
	schedule->redraw = false;
	// an accepted submission starts a new callback cycle right away
	schedule->callbacks_due = true;
	schedule->callbacks_wake = 0;
}

void schedule_queued(struct schedule *schedule, void *ticket) {
	// a successor doesn't wake clients, its promotion does
	schedule->queued = ticket;
	schedule->redraw = false;
}

void schedule_empty(struct schedule *schedule, int64_t now) {
	schedule->redraw = false;
	// one wake on the refresh grid. a cycle that's already due or armed
	// stays as it is, a late wake doesn't move the grid
	if (!schedule->callbacks_due && !schedule->callbacks_wake) {
		schedule->callbacks_wake = schedule_next_vblank(schedule, now);
	}
}

void *schedule_presented(struct schedule *schedule, int64_t kernel, int64_t now) {
	if (!schedule->pending) {
		return NULL; // nobody waited for this one, the clock stays
	}
	schedule->vblank = kernel > 0 ? kernel : now;
	schedule->have_vblank = true;
	void *ticket = schedule->pending;
	schedule->pending = NULL;
	return ticket;
}

void schedule_promoted(struct schedule *schedule, int64_t submit) {
	schedule->pending = schedule->queued;
	schedule->queued = NULL;
	// its own watchdog, starting from the real handoff
	schedule->pending_submit = submit;
	schedule->callbacks_due = true;
	schedule->callbacks_wake = 0;
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
	return schedule->active && (schedule->callbacks_due
		|| (schedule->callbacks_wake && now >= schedule->callbacks_wake));
}

void schedule_callbacks_sent(struct schedule *schedule) {
	schedule->callbacks_due = false;
	schedule->callbacks_wake = 0;
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
	if (schedule->callbacks_due) {
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
	schedule->callbacks_due = false;
	schedule->callbacks_wake = 0;
	schedule->active = false;
}

void schedule_resume(struct schedule *schedule) {
	schedule->active = true;
	schedule->have_vblank = false;
	schedule->redraw = true;
	schedule->callbacks_due = false;
	schedule->callbacks_wake = 0;
	reset_budget(schedule);
}
