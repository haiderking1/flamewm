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
	schedule_submitted(&s, &a, 0);
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
	schedule_submitted(&s, &a, -MS);
	CHECK(schedule_presented(&s, 0, 0) == &a);
	schedule_submitted(&s, &b, 1 * MS);
	schedule_callbacks_sent(&s); // a submission's callbacks are due at once
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
	schedule_submitted(&s, &a, 0);
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
	schedule_submitted(&s, &a, 0);
	schedule_queued(&s, &b);
	CHECK(schedule_presented(&s, PERIOD, PERIOD) == &a);
	CHECK(schedule_discard_queued(&s) == &b);
	CHECK(!s.pending && !s.queued && s.redraw);
	CHECK(schedule_render_due(&s, PERIOD));
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
	schedule_submitted(&s, &a, 0);
	schedule_presented(&s, PERIOD, PERIOD);
	schedule_callbacks_sent(&s);
	// nothing pending, nothing asked for: nothing to wake up for
	CHECK(schedule_deadline(&s, PERIOD) == 0);
	// a pending flip without a request: only its watchdog
	schedule_request_redraw(&s);
	schedule_submitted(&s, &b, 2 * PERIOD);
	schedule_callbacks_sent(&s);
	CHECK(schedule_deadline(&s, 2 * PERIOD) == 2 * PERIOD + SCHEDULE_WATCHDOG_NS);
}

static void test_callbacks(void) {
	struct schedule s;
	schedule_init(&s, SCHEDULE_IMMEDIATE, PERIOD);
	// a submission starts a cycle at once
	schedule_submitted(&s, &a, 0);
	CHECK(schedule_callbacks_due(&s, 0));
	schedule_callbacks_sent(&s);
	CHECK(!schedule_callbacks_due(&s, 0));
	schedule_presented(&s, PERIOD, PERIOD);
	// nothing changed: one wake on the grid, not now, not a pageflip
	schedule_request_redraw(&s);
	schedule_empty(&s, PERIOD + MS);
	CHECK(!schedule_callbacks_due(&s, PERIOD + MS));
	CHECK(s.callbacks_wake == 2 * PERIOD);
	CHECK(schedule_deadline(&s, PERIOD + MS) == 2 * PERIOD);
	// another empty pass doesn't move it
	schedule_empty(&s, PERIOD + 2 * MS);
	CHECK(s.callbacks_wake == 2 * PERIOD);
	// late wakes still deliver, then it's gone
	CHECK(schedule_callbacks_due(&s, 2 * PERIOD + 3 * MS));
	schedule_callbacks_sent(&s);
	CHECK(schedule_deadline(&s, 3 * PERIOD) == 0);

	// a successor's preparation doesn't wake clients, its promotion does
	schedule_init(&s, SCHEDULE_DEADLINE, PERIOD);
	schedule_submitted(&s, &a, 0);
	schedule_callbacks_sent(&s);
	schedule_queued(&s, &b);
	CHECK(!schedule_callbacks_due(&s, MS));
	schedule_presented(&s, PERIOD, PERIOD);
	CHECK(!schedule_callbacks_due(&s, PERIOD));
	schedule_promoted(&s, PERIOD);
	CHECK(schedule_callbacks_due(&s, PERIOD));
	// an empty pass never downgrades a cycle that's already due
	schedule_empty(&s, PERIOD);
	CHECK(schedule_callbacks_due(&s, PERIOD) && s.callbacks_wake == 0);
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
	schedule_submitted(&s, &a, 0);
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
	schedule_submitted(&s, &a, 0);
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
	schedule_submitted(&s, &c, 2 * MS);
	CHECK(s.pending == &c);
}

int main(void) {
	test_policy_parse();
	test_immediate_waits_for_the_slot();
	test_deadline_window();
	test_one_successor_only();
	test_discarded_successor_asks_again();
	test_unexpected_flip();
	test_idle_has_no_timer();
	test_callbacks();
	test_estimates();
	test_adaptive_hysteresis();
	test_watchdog();
	test_pause_resume();
	if (failures) {
		fprintf(stderr, "%d failed\n", failures);
		return 1;
	}
	printf("schedule: all good\n");
	return 0;
}
