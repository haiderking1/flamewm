#ifndef FLAME_RENDER_SCHEDULE_H
#define FLAME_RENDER_SCHEDULE_H

#include <stdbool.h>
#include <stdint.h>

// raven's display deadline scheduler. it owns opaque frame tickets and
// knows nothing about wayland, buffers, fences or KMS. there's one KMS
// pending slot and at most one rendered successor, never a backlog.
// times are CLOCK_MONOTONIC nanoseconds
//
// policies (FLAME_FRAME_PIPELINE):
// - immediate: render only when the KMS slot is empty, what wlroots did
// - adaptive (default): immediate while frames are cheap. once the
//   estimated render work reaches 3/4 of a refresh it may prepare one
//   successor near the predicted pageflip, below 1/2 it goes back
// - deadline: always that overlap window, for comparing

// frame callbacks come in cycles. a new one (a submission, or the refresh
// grid moving on) gives every visible surface one turn, the current one only
// catches surfaces that haven't had theirs yet, so nobody wakes twice a refresh
enum schedule_cycle {
	SCHEDULE_CYCLE_NONE,
	SCHEDULE_CYCLE_CURRENT,
	SCHEDULE_CYCLE_NEW,
};

enum schedule_policy {
	SCHEDULE_ADAPTIVE,
	SCHEDULE_IMMEDIATE,
	SCHEDULE_DEADLINE,
};

// a lagging client or driver doesn't get to hold the pending slot longer
// than this without it being reported
#define SCHEDULE_WATCHDOG_NS 3000000000LL
// dispatch and submission jitter on top of the measured render work
#define SCHEDULE_MARGIN_NS 500000LL

struct schedule {
	enum schedule_policy policy;
	bool overlap; // adaptive is rendering ahead right now
	int64_t period; // one refresh

	// the display grid: a known vblank, once the first flip says one
	bool have_vblank;
	int64_t vblank;

	// the KMS slot and the successor waiting behind it
	void *pending;
	int64_t pending_submit;
	void *queued;
	// someone wants a frame, one request no matter how many asked
	bool redraw;
	bool active; // off while the session is away

	// render lead estimates, see schedule_observe_*
	int64_t cpu, gpu;

	// frame callbacks: a cycle due right now, and a wake on the refresh grid
	enum schedule_cycle cycle;
	int64_t callbacks_wake;
};

// NULL picks adaptive. false for anything that isn't exactly
// adaptive/immediate/deadline
bool schedule_policy_parse(const char *value, enum schedule_policy *policy);
const char *schedule_policy_name(enum schedule_policy policy);

void schedule_init(struct schedule *schedule, enum schedule_policy policy, int64_t period);
// a new mode, the grid starts over
void schedule_set_period(struct schedule *schedule, int64_t period);

void schedule_request_redraw(struct schedule *schedule);
// a redraw was asked for and the window to render it is open
bool schedule_render_due(const struct schedule *schedule, int64_t now);
// what one frame is estimated to cost plus the margin, capped at a refresh
int64_t schedule_lead(const struct schedule *schedule);
// is it rendering ahead of the pending flip (deadline, or adaptive under load)
bool schedule_overlapping(const struct schedule *schedule);
// the first vblank after t on the predicted grid
int64_t schedule_next_vblank(const struct schedule *schedule, int64_t t);

// what came of a due render. submitted: the KMS slot was empty and the frame
// went to KMS at now. queued: rendered behind the pending one, the caller
// holds it until schedule_presented. empty: nothing changed, nothing goes to
// KMS, callbacks go out on the next refresh instead
void schedule_submitted(struct schedule *schedule, void *ticket, int64_t now);
void schedule_queued(struct schedule *schedule, void *ticket);
void schedule_empty(struct schedule *schedule, int64_t now);

// the pending frame flipped. kernel is its timestamp (0 when unknown, now is
// used without claiming hardware accuracy), the grid only ever moves forward.
// returns the finished ticket, NULL for a flip nobody was waiting for. a
// queued successor stays queued: submit it, then schedule_promoted with the
// actual submit time. without one the current callback cycle comes due again
void *schedule_presented(struct schedule *schedule, int64_t kernel, int64_t now);
void schedule_promoted(struct schedule *schedule, int64_t submit);
// the successor's submission failed right away: it's returned, a fresh
// render is requested. never for an accepted submission
void *schedule_discard_queued(struct schedule *schedule);

// render wall time / gpu elapsed time of a changed frame
void schedule_observe_cpu(struct schedule *schedule, int64_t ns);
void schedule_observe_gpu(struct schedule *schedule, int64_t ns);

bool schedule_callbacks_due(const struct schedule *schedule, int64_t now);
// the due callbacks start a new cycle (true) or revisit the current one
bool schedule_callbacks_new_cycle(const struct schedule *schedule, int64_t now);
void schedule_callbacks_sent(struct schedule *schedule, int64_t now);

// the earliest moment something is due: the render window, a callback wake
// or the watchdog. 0 for nothing at all
int64_t schedule_deadline(const struct schedule *schedule, int64_t now);
// the pending frame has been waiting longer than the watchdog allows
bool schedule_stalled(const struct schedule *schedule, int64_t now);

// session away: both tickets are dropped (returned through the out
// pointers), nothing is due. back: start clean with a fresh render
void schedule_pause(struct schedule *schedule, void **pending, void **queued);
void schedule_resume(struct schedule *schedule);

#endif
