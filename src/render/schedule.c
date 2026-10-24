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
	schedule->margin = SCHEDULE_MARGIN_NS;
	schedule->extra = 0;
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
	period = period > 0 ? period : FALLBACK_PERIOD_NS;
	if (schedule->period && period != schedule->period) {
		// another refresh rate: what was learnt at the old one (a margin
		// grown at 60Hz is half a 165Hz refresh) starts over
		reset_budget(schedule);
		schedule->pending_target = 0;
	}
	schedule->period = period;
	schedule->have_vblank = false;
}

void schedule_request_redraw(struct schedule *schedule) {
	schedule->redraw = true;
}

void schedule_set_asap(struct schedule *schedule, bool asap) {
	schedule->asap = asap;
}

void schedule_mark_off_grid(struct schedule *schedule) {
	schedule->pending_off_grid = true;
	schedule->pending_target = 0;
}

static int64_t base_lead(const struct schedule *schedule) {
	int64_t lead = schedule->cpu + schedule->gpu + schedule->margin;
	return lead < schedule->period ? lead : schedule->period;
}

int64_t schedule_lead(const struct schedule *schedule) {
	int64_t lead = base_lead(schedule) + schedule->extra;
	return lead < schedule->period ? lead : schedule->period;
}

void schedule_set_extra(struct schedule *schedule, int64_t ns) {
	schedule->extra = ns > 0 ? ns : 0;
}

bool schedule_overlapping(const struct schedule *schedule) {
	if (schedule->asap) {
		return false; // the next frame goes when this one's flipped, not before
	}
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

// the repaint window: with the slot free, render as late as the next vblank
// allows, not the moment something changed. everything that comes in before
// then (an app's frame drawn right after the flip, the pointer, an X11
// present) makes that refresh instead of the one after. past the window it's
// now, still aiming at that vblank. no grid yet, or immediate: now
static int64_t repaint_window(const struct schedule *schedule, int64_t now) {
	if (!schedule->have_vblank || schedule->policy == SCHEDULE_IMMEDIATE || schedule->asap) {
		return now;
	}
	int64_t window = schedule_next_vblank(schedule, now) - schedule_lead(schedule);
	return window > now ? window : now;
}

int64_t schedule_render_at(const struct schedule *schedule, int64_t now) {
	if (!schedule->have_vblank || schedule->policy == SCHEDULE_IMMEDIATE || schedule->asap) {
		return 0;
	}
	// a pending frame holds the slot until its flip, the next one after that
	int64_t from = schedule->pending
		? schedule_next_vblank(schedule, schedule->pending_submit) : now;
	return schedule_next_vblank(schedule, from) - schedule_lead(schedule);
}

bool schedule_render_due(const struct schedule *schedule, int64_t now) {
	if (!schedule->active || !schedule->redraw || schedule->queued) {
		return false;
	}
	if (!schedule->pending) {
		return now >= repaint_window(schedule, now);
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

void schedule_submitted(struct schedule *schedule, void *ticket, int64_t started, int64_t now) {
	schedule->pending = ticket;
	schedule->pending_submit = now;
	schedule->pending_off_grid = false;
	// a render that ran past its vblank counts as a miss, not as aimed at
	// the one after. one that started with less time left than a render
	// takes (something came in right before a vblank) never had a chance at
	// it: it's aimed at the one after
	schedule->pending_target = 0;
	if (schedule->have_vblank) {
		int64_t target = schedule_next_vblank(schedule, started);
		if (target - started < schedule->cpu + schedule->gpu) {
			target += schedule->period;
		}
		schedule->pending_target = target;
	}
	schedule->redraw = false;
	// nobody wakes yet: a client drawing now commits while this frame waits
	// for its flip and makes the one after. the flip wakes them (apps too
	// slow for that get theirs early, see feedback_frame_done_early)
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
	if (schedule->pending_off_grid) {
		// a tearing flip goes when it's ready, not on a vblank: the grid
		// stays where the last real one put it, nothing was missed
		schedule->pending_off_grid = false;
		schedule->last_missed = false;
		void *ticket = schedule->pending;
		schedule->pending = NULL;
		new_cycle(schedule);
		return ticket;
	}
	// came a refresh (or more) after the vblank it was meant for: the window
	// was too tight. wider at once, then back down slowly while flips land
	schedule->last_missed = kernel > 0 && schedule->pending_target
		&& kernel > schedule->pending_target + schedule->period / 2;
	if (schedule->last_missed) {
		schedule->margin += SCHEDULE_MARGIN_NS;
		if (schedule->margin > schedule->period / 2) {
			schedule->margin = schedule->period / 2;
		}
	} else if (schedule->margin > SCHEDULE_MARGIN_NS) {
		schedule->margin -= (schedule->margin - SCHEDULE_MARGIN_NS) / 64 + 1;
	}
	// a late or out of order timestamp never drags the grid backwards
	if (!schedule->have_vblank || anchor > schedule->vblank) {
		schedule->vblank = anchor;
		schedule->have_vblank = true;
	}
	void *ticket = schedule->pending;
	schedule->pending = NULL;
	// the flip is what clients wait for: draw now, commit, the slot is free
	// and the frame makes the very next refresh
	new_cycle(schedule);
	return ticket;
}

void schedule_promoted(struct schedule *schedule, int64_t submit) {
	schedule->pending = schedule->queued;
	schedule->queued = NULL;
	schedule->pending_off_grid = false;
	// its own watchdog, starting from the real handoff
	schedule->pending_submit = submit;
	schedule->pending_target = schedule->have_vblank ? schedule_next_vblank(schedule, submit) : 0;
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
	// a one-off stall planned for doesn't make the pipeline go deeper
	int64_t lead = base_lead(schedule);
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

void schedule_callbacks_next(struct schedule *schedule, int64_t now) {
	if (schedule->active && !schedule->callbacks_wake) {
		schedule->callbacks_wake = schedule_next_vblank(schedule, now);
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
			earliest(&best, repaint_window(schedule, now));
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
