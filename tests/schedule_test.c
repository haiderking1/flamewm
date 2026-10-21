// the scheduler is pure logic, so it gets tested on its own: make test
#include <stdio.h>
#include <stdlib.h>

#include "render/schedule.h"

#define MS 1000000LL
#define PERIOD 6060606LL // 165hz

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

static int a, b, c; // tickets, only their addresses matter

static void test_policy_parse(void) {
	enum schedule_policy policy;
	CHECK(schedule_policy_parse(NULL, &policy) && policy == SCHEDULE_ADAPTIVE);
	CHECK(schedule_policy_parse("immediate", &policy) && policy == SCHEDULE_IMMEDIATE);
	CHECK(schedule_policy_parse("deadline", &policy) && policy == SCHEDULE_DEADLINE);
	CHECK(schedule_policy_parse("adaptive", &policy) && policy == SCHEDULE_ADAPTIVE);
	CHECK(!schedule_policy_parse("", &policy));
	CHECK(!schedule_policy_parse("Deadline", &policy));
	CHECK(!schedule_policy_parse("immediately", &policy));
}

static void test_immediate_waits_for_the_slot(void) {
	struct schedule s;
	schedule_init(&s, SCHEDULE_IMMEDIATE, PERIOD);
	CHECK(schedule_render_due(&s, 0)); // startup frame
	schedule_submitted(&s, &a, 0, 0);
	schedule_request_redraw(&s);
	// pending flip: no overlap in immediate, however late it gets
	CHECK(!schedule_render_due(&s, PERIOD - 1));
	CHECK(!schedule_render_due(&s, 100 * MS));
	CHECK(schedule_presented(&s, PERIOD, PERIOD) == &a);
	CHECK(schedule_render_due(&s, PERIOD));
}

static void test_deadline_window(void) {
	struct schedule s;
	schedule_init(&s, SCHEDULE_DEADLINE, PERIOD);
	// a grid from a first flip at t=0
	schedule_submitted(&s, &a, -MS, -MS);
	CHECK(schedule_presented(&s, 0, 0) == &a);
	schedule_submitted(&s, &b, 1 * MS, 1 * MS);
	schedule_callbacks_sent(&s, 1 * MS); // the flip's callbacks went out
	schedule_request_redraw(&s);
	int64_t lead = schedule_lead(&s); // 1 + 1 + 0.5ms to start
	CHECK(lead == 2500000LL);
	int64_t window = PERIOD - lead; // next vblank after the submission, minus lead
	CHECK(!schedule_render_due(&s, window - 1));
	CHECK(schedule_render_due(&s, window));
	CHECK(schedule_deadline(&s, 2 * MS) == window);
	// a missed window stays open rather than chasing the next refresh
	CHECK(schedule_render_due(&s, window + 3 * PERIOD));
}

static void test_one_successor_only(void) {
	struct schedule s;
	schedule_init(&s, SCHEDULE_DEADLINE, PERIOD);
	schedule_submitted(&s, &a, 0, 0);
	schedule_request_redraw(&s);
	CHECK(schedule_render_due(&s, PERIOD));
	schedule_queued(&s, &b);
	// more commits, still one request, nothing renders behind a successor
	schedule_request_redraw(&s);
	schedule_request_redraw(&s);
	CHECK(!schedule_render_due(&s, 10 * PERIOD));
	CHECK(schedule_presented(&s, PERIOD, PERIOD) == &a);
	CHECK(s.queued == &b && !s.pending);
	schedule_promoted(&s, PERIOD + 100);
	CHECK(s.pending == &b && !s.queued && s.pending_submit == PERIOD + 100);
	// the coalesced request survived
	CHECK(s.redraw);
}

static void test_discarded_successor_asks_again(void) {
	struct schedule s;
	schedule_init(&s, SCHEDULE_DEADLINE, PERIOD);
	schedule_submitted(&s, &a, 0, 0);
	schedule_queued(&s, &b);
	CHECK(schedule_presented(&s, PERIOD, PERIOD) == &a);
	CHECK(schedule_discard_queued(&s) == &b);
	CHECK(!s.pending && !s.queued && s.redraw);
	// asks again, in the window before the next vblank like any frame
	CHECK(!schedule_render_due(&s, PERIOD));
	CHECK(schedule_render_due(&s, 2 * PERIOD - schedule_lead(&s)));
}

static void test_missed_vblank_widens_margin(void) {
	struct schedule s;
	schedule_init(&s, SCHEDULE_ADAPTIVE, PERIOD);
	schedule_submitted(&s, &a, 0, 0);
	schedule_presented(&s, PERIOD, PERIOD); // grid at PERIOD
	int64_t before = schedule_lead(&s);
	// submitted for 2 * PERIOD, flipped a refresh late
	schedule_submitted(&s, &a, 2 * PERIOD - 3 * MS, 2 * PERIOD - MS);
	schedule_presented(&s, 3 * PERIOD, 3 * PERIOD);
	CHECK(s.last_missed);
	CHECK(schedule_lead(&s) == before + SCHEDULE_MARGIN_NS);
	// on time from then on: it narrows again, slowly
	int64_t wide = s.margin;
	for (int i = 4; i < 20; i++) {
		schedule_submitted(&s, &a, i * PERIOD - 3 * MS, i * PERIOD - MS);
		schedule_presented(&s, i * PERIOD, i * PERIOD);
		CHECK(!s.last_missed);
	}
	CHECK(s.margin < wide && s.margin > SCHEDULE_MARGIN_NS);
}

// something came in 0.1ms before a vblank: no render makes that one, the
// flip a refresh later is on time
static void test_late_start_is_no_miss(void) {
	struct schedule s;
	schedule_init(&s, SCHEDULE_ADAPTIVE, PERIOD);
	schedule_submitted(&s, &a, 0, 0);
	schedule_presented(&s, PERIOD, PERIOD);
	schedule_submitted(&s, &a, 2 * PERIOD - MS / 10, 2 * PERIOD + MS / 10);
	schedule_presented(&s, 3 * PERIOD, 3 * PERIOD);
	CHECK(!s.last_missed);
}

// someone asked again after its turn and nothing flips: the next vblank
// starts a new cycle anyway, once
static void test_callbacks_next_cycle_without_flip(void) {
	struct schedule s;
	schedule_init(&s, SCHEDULE_ADAPTIVE, PERIOD);
	schedule_submitted(&s, &a, 0, 0);
	schedule_presented(&s, PERIOD, PERIOD);
	CHECK(schedule_callbacks_due(&s, PERIOD));
	schedule_callbacks_sent(&s, PERIOD);
	CHECK(!schedule_callbacks_due(&s, PERIOD + MS));
	schedule_callbacks_next(&s, PERIOD + MS);
	CHECK(schedule_deadline(&s, PERIOD + MS) == 2 * PERIOD);
	CHECK(!schedule_callbacks_due(&s, 2 * PERIOD - 1));
	CHECK(schedule_callbacks_due(&s, 2 * PERIOD));
	CHECK(schedule_callbacks_new_cycle(&s, 2 * PERIOD));
	schedule_callbacks_sent(&s, 2 * PERIOD);
	CHECK(!schedule_callbacks_due(&s, 2 * PERIOD + MS));
}

// a render that started for one vblank and only reached KMS after it lands a
// refresh late: that's a miss, not a frame aimed at the next one
static void test_render_past_its_vblank_is_a_miss(void) {
	struct schedule s;
	schedule_init(&s, SCHEDULE_ADAPTIVE, PERIOD);
	schedule_submitted(&s, &a, 0, 0);
	schedule_presented(&s, PERIOD, PERIOD);
	schedule_submitted(&s, &a, 2 * PERIOD - 3 * MS, 2 * PERIOD + MS / 4);
	schedule_presented(&s, 3 * PERIOD, 3 * PERIOD);
	CHECK(s.last_missed);
}

static void test_repaint_window(void) {
	struct schedule s;
	schedule_init(&s, SCHEDULE_ADAPTIVE, PERIOD);
	// no grid before the first flip: right away
	CHECK(schedule_render_due(&s, 0));
	schedule_submitted(&s, &a, 0, 0);
	CHECK(schedule_presented(&s, PERIOD, PERIOD) == &a);
	schedule_callbacks_sent(&s, PERIOD);
	// a change right after the flip waits until just before the next vblank,
	// so whatever comes in meanwhile makes that refresh too
	schedule_request_redraw(&s);
	int64_t window = 2 * PERIOD - schedule_lead(&s);
	CHECK(!schedule_render_due(&s, PERIOD + MS));
	CHECK(schedule_deadline(&s, PERIOD + MS) == window);
	CHECK(schedule_render_due(&s, window));
	// past the window: now, still aiming at that vblank
	CHECK(schedule_render_due(&s, window + MS));
	// immediate doesn't wait
	schedule_init(&s, SCHEDULE_IMMEDIATE, PERIOD);
	schedule_submitted(&s, &a, 0, 0);
	schedule_presented(&s, PERIOD, PERIOD);
	schedule_request_redraw(&s);
	CHECK(schedule_render_due(&s, PERIOD + MS));
}

static void test_unexpected_flip(void) {
	struct schedule s;
	schedule_init(&s, SCHEDULE_ADAPTIVE, PERIOD);
	CHECK(schedule_presented(&s, 123, 123) == NULL);
	CHECK(!s.have_vblank); // the clock didn't move
}

static void test_idle_has_no_timer(void) {
	struct schedule s;
	schedule_init(&s, SCHEDULE_DEADLINE, PERIOD);
	schedule_submitted(&s, &a, 0, 0);
	schedule_callbacks_sent(&s, 0);
	schedule_presented(&s, PERIOD, PERIOD);
	schedule_callbacks_sent(&s, PERIOD); // the flip revisited the cycle
	// nothing pending, nothing asked for: nothing to wake up for
	CHECK(schedule_deadline(&s, PERIOD) == 0);
	// a pending flip without a request: only its watchdog
	schedule_request_redraw(&s);
	schedule_submitted(&s, &b, 2 * PERIOD, 2 * PERIOD);
	schedule_callbacks_sent(&s, 2 * PERIOD);
	CHECK(schedule_deadline(&s, 2 * PERIOD) == 2 * PERIOD + SCHEDULE_WATCHDOG_NS);
}

static void test_callbacks(void) {
	struct schedule s;
	schedule_init(&s, SCHEDULE_IMMEDIATE, PERIOD);
	// a submission wakes nobody, its flip starts the new cycle
	schedule_submitted(&s, &a, 0, 0);
	CHECK(!schedule_callbacks_due(&s, 0));
	schedule_presented(&s, PERIOD, PERIOD);
	CHECK(schedule_callbacks_due(&s, PERIOD) && schedule_callbacks_new_cycle(&s, PERIOD));
	schedule_callbacks_sent(&s, PERIOD);
	CHECK(!schedule_callbacks_due(&s, PERIOD));
	// nothing changed: one wake on the grid, not a pageflip
	schedule_request_redraw(&s);
	schedule_empty(&s, PERIOD + MS);
	CHECK(s.callbacks_wake == 2 * PERIOD);
	CHECK(!schedule_callbacks_new_cycle(&s, PERIOD + MS)); // revisit now
	schedule_callbacks_sent(&s, PERIOD + MS);
	CHECK(schedule_deadline(&s, PERIOD + MS) == 2 * PERIOD);
	// another empty pass in the same refresh keeps the same wake
	schedule_empty(&s, PERIOD + 2 * MS);
	CHECK(s.callbacks_wake == 2 * PERIOD);
	schedule_callbacks_sent(&s, PERIOD + 2 * MS);
	// the grid moved on: that's a new cycle, even delivered late
	CHECK(schedule_callbacks_due(&s, 2 * PERIOD + 3 * MS));
	CHECK(schedule_callbacks_new_cycle(&s, 2 * PERIOD + 3 * MS));
	schedule_callbacks_sent(&s, 2 * PERIOD + 3 * MS);
	CHECK(schedule_deadline(&s, 3 * PERIOD) == 0);
	// an empty pass landing after the wake expired starts the new cycle too
	schedule_empty(&s, 3 * PERIOD + MS);
	schedule_callbacks_sent(&s, 3 * PERIOD + MS);
	CHECK(s.callbacks_wake == 4 * PERIOD);
	schedule_empty(&s, 4 * PERIOD + MS);
	CHECK(schedule_callbacks_new_cycle(&s, 4 * PERIOD + MS));

	// a successor's preparation doesn't wake clients, the flip it goes in on does
	schedule_init(&s, SCHEDULE_DEADLINE, PERIOD);
	schedule_submitted(&s, &a, 0, 0);
	schedule_queued(&s, &b);
	CHECK(!schedule_callbacks_due(&s, MS));
	schedule_presented(&s, PERIOD, PERIOD);
	CHECK(schedule_callbacks_new_cycle(&s, PERIOD));
	schedule_promoted(&s, PERIOD);
	CHECK(schedule_callbacks_new_cycle(&s, PERIOD));
	// an empty pass never downgrades a cycle that's already new
	schedule_empty(&s, PERIOD);
	CHECK(schedule_callbacks_new_cycle(&s, PERIOD));
}

static void test_clock_only_moves_forward(void) {
	struct schedule s;
	schedule_init(&s, SCHEDULE_DEADLINE, PERIOD);
	schedule_submitted(&s, &a, 0, 0);
	schedule_presented(&s, 10 * PERIOD, 10 * PERIOD);
	CHECK(s.vblank == 10 * PERIOD);
	schedule_submitted(&s, &b, 10 * PERIOD + MS, 10 * PERIOD + MS);
	// a stale timestamp from an older flip doesn't drag the grid back
	schedule_presented(&s, 5 * PERIOD, 11 * PERIOD);
	CHECK(s.vblank == 10 * PERIOD);
	CHECK(schedule_next_vblank(&s, 11 * PERIOD + MS) == 12 * PERIOD);
}

static void test_estimates(void) {
	struct schedule s;
	schedule_init(&s, SCHEDULE_ADAPTIVE, PERIOD);
	schedule_observe_cpu(&s, 3 * MS);
	CHECK(s.cpu == 3 * MS); // bigger: at once
	schedule_observe_cpu(&s, 1 * MS);
	CHECK(s.cpu == 3 * MS - 2 * MS / 16); // smaller: 1/16 of the gap
	schedule_observe_gpu(&s, 100 * MS);
	CHECK(s.gpu == PERIOD); // capped at a refresh
	CHECK(schedule_lead(&s) == PERIOD); // lead capped too
	schedule_observe_gpu(&s, -1);
	CHECK(s.gpu == PERIOD); // missing samples change nothing
}

static void test_adaptive_hysteresis(void) {
	struct schedule s;
	schedule_init(&s, SCHEDULE_ADAPTIVE, PERIOD);
	CHECK(!schedule_overlapping(&s));
	// lead = cpu + 1ms gpu + 0.5ms: 3/4 of a refresh is ~4.55ms
	schedule_observe_cpu(&s, 3 * MS);
	CHECK(!schedule_overlapping(&s)); // 4.5ms
	schedule_observe_cpu(&s, 3100000);
	CHECK(schedule_overlapping(&s)); // 4.6ms
	// back down below 3/4 but above 1/2 (3.03ms): stays on
	for (int i = 0; i < 4; i++) {
		schedule_observe_cpu(&s, 2 * MS);
	}
	CHECK(schedule_lead(&s) > PERIOD / 2 && schedule_overlapping(&s));
	for (int i = 0; i < 100; i++) {
		schedule_observe_cpu(&s, 0);
	}
	CHECK(!schedule_overlapping(&s));
	// immediate and deadline don't care
	schedule_init(&s, SCHEDULE_IMMEDIATE, PERIOD);
	schedule_observe_cpu(&s, 100 * MS);
	CHECK(!schedule_overlapping(&s));
}

static void test_watchdog(void) {
	struct schedule s;
	schedule_init(&s, SCHEDULE_DEADLINE, PERIOD);
	schedule_submitted(&s, &a, 0, 0);
	CHECK(!schedule_stalled(&s, SCHEDULE_WATCHDOG_NS));
	CHECK(schedule_stalled(&s, SCHEDULE_WATCHDOG_NS + 1));
	// a successor never restarts the old one's watchdog
	schedule_queued(&s, &b);
	CHECK(s.pending_submit == 0);
	// promotion gives it a fresh one, even after a late flip event
	schedule_presented(&s, 5 * SCHEDULE_WATCHDOG_NS, 5 * SCHEDULE_WATCHDOG_NS);
	schedule_promoted(&s, 5 * SCHEDULE_WATCHDOG_NS);
	CHECK(!schedule_stalled(&s, 5 * SCHEDULE_WATCHDOG_NS + MS));
}

static void test_pause_resume(void) {
	struct schedule s;
	schedule_init(&s, SCHEDULE_DEADLINE, PERIOD);
	schedule_submitted(&s, &a, 0, 0);
	schedule_queued(&s, &b);
	schedule_observe_cpu(&s, 5 * MS);
	void *pending, *queued;
	schedule_pause(&s, &pending, &queued);
	CHECK(pending == &a && queued == &b);
	CHECK(!schedule_render_due(&s, MS) && schedule_deadline(&s, MS) == 0);
	schedule_request_redraw(&s);
	CHECK(!schedule_render_due(&s, MS)); // away means away
	schedule_resume(&s);
	CHECK(schedule_render_due(&s, 2 * MS) && s.cpu == MS && !s.have_vblank);
	schedule_submitted(&s, &c, 2 * MS, 2 * MS);
	CHECK(s.pending == &c);
}

// tearing, vrr: a frame goes the moment something changed, no repaint
// window, and nothing renders ahead of the pending flip whatever the policy
static void test_asap(void) {
	struct schedule s;
	schedule_init(&s, SCHEDULE_DEADLINE, PERIOD);
	schedule_submitted(&s, &a, -MS, -MS);
	CHECK(schedule_presented(&s, 0, 0) == &a);
	schedule_set_asap(&s, true);
	schedule_request_redraw(&s);
	CHECK(schedule_render_due(&s, 1 * MS)); // not at the window before the vblank
	CHECK(schedule_render_at(&s, 1 * MS) == 0);
	schedule_submitted(&s, &b, 1 * MS, 1 * MS);
	schedule_request_redraw(&s);
	CHECK(!schedule_overlapping(&s));
	CHECK(!schedule_render_due(&s, PERIOD - MS)); // waits for the flip, deadline or not
	CHECK(schedule_presented(&s, 2 * MS, 2 * MS) == &b);
	CHECK(schedule_render_due(&s, 2 * MS));
	// back to vsync: the repaint window again
	schedule_set_asap(&s, false);
	CHECK(!schedule_render_due(&s, 2 * MS + 1));
}

// a tearing flip lands whenever: the grid stays put, nothing counts as missed
static void test_off_grid_flip(void) {
	struct schedule s;
	schedule_init(&s, SCHEDULE_ADAPTIVE, PERIOD);
	schedule_submitted(&s, &a, -MS, -MS);
	CHECK(schedule_presented(&s, 0, 0) == &a);
	int64_t margin = s.margin;
	schedule_submitted(&s, &b, 1 * MS, 1 * MS);
	schedule_mark_off_grid(&s);
	// a whole refresh and a half after: a miss for a vsync'd flip, not for this one
	CHECK(schedule_presented(&s, PERIOD + PERIOD / 2 + MS, PERIOD + PERIOD / 2 + MS) == &b);
	CHECK(!s.last_missed);
	CHECK(s.margin == margin);
	CHECK(s.vblank == 0); // the grid didn't move
	CHECK(schedule_next_vblank(&s, 3 * MS) == PERIOD);
	CHECK(s.cycle == SCHEDULE_CYCLE_NEW); // clients still wake on it
	// the next real flip anchors again
	schedule_submitted(&s, &c, 2 * PERIOD + MS, 2 * PERIOD + MS);
	CHECK(schedule_presented(&s, 3 * PERIOD, 3 * PERIOD) == &c);
	CHECK(s.vblank == 3 * PERIOD);
}

int main(void) {
	test_policy_parse();
	test_immediate_waits_for_the_slot();
	test_deadline_window();
	test_one_successor_only();
	test_discarded_successor_asks_again();
	test_repaint_window();
	test_missed_vblank_widens_margin();
	test_render_past_its_vblank_is_a_miss();
	test_callbacks_next_cycle_without_flip();
	test_late_start_is_no_miss();
	test_unexpected_flip();
	test_idle_has_no_timer();
	test_callbacks();
	test_clock_only_moves_forward();
	test_estimates();
	test_adaptive_hysteresis();
	test_watchdog();
	test_pause_resume();
	test_asap();
	test_off_grid_flip();
	if (failures) {
		fprintf(stderr, "%d failed\n", failures);
		return 1;
	}
	printf("schedule: all good\n");
	return 0;
}
