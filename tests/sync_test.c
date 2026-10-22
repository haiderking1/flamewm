// how frames meet the display (tearing, vrr) is pure logic, tested on its
// own: make test
#include <stdio.h>

#include "render/sync.h"

#define MS 1000000LL

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

static void test_tearing_wanted(void) {
	struct sync s;
	sync_init(&s);
	CHECK(sync_tearing_wanted(&s, true, true, false));
	CHECK(!sync_tearing_wanted(&s, false, true, false)); // the config says no
	CHECK(!sync_tearing_wanted(&s, true, false, false)); // the app didn't ask
	CHECK(!sync_tearing_wanted(&s, true, true, true)); // the cursor plane changes too
}

// refused now and then is fine, a run of them turns it off till a reset
static void test_tearing_refusals(void) {
	struct sync s;
	sync_init(&s);
	for (int i = 0; i < SYNC_TEARING_REFUSALS - 1; i++) {
		sync_tearing_result(&s, true);
	}
	CHECK(sync_tearing_wanted(&s, true, true, false));
	sync_tearing_result(&s, false); // one went through: the run starts over
	for (int i = 0; i < SYNC_TEARING_REFUSALS - 1; i++) {
		sync_tearing_result(&s, true);
	}
	CHECK(sync_tearing_wanted(&s, true, true, false));
	sync_tearing_result(&s, true);
	CHECK(!sync_tearing_wanted(&s, true, true, false));
	sync_reset(&s); // a new mode or back from a vt: another chance
	CHECK(sync_tearing_wanted(&s, true, true, false));
}

static void test_vrr_policy(void) {
	struct sync s;
	sync_init(&s);
	CHECK(!sync_vrr_wanted(&s, SYNC_VRR_OFF, true, true, 0));
	CHECK(sync_vrr_wanted(&s, SYNC_VRR_ALWAYS, true, false, 0));
	CHECK(!sync_vrr_wanted(&s, SYNC_VRR_ALWAYS, false, false, 0)); // the output can't
	sync_vrr_refused(&s);
	CHECK(!sync_vrr_wanted(&s, SYNC_VRR_ALWAYS, true, false, 0)); // KMS said no
	sync_reset(&s);
	CHECK(sync_vrr_wanted(&s, SYNC_VRR_ALWAYS, true, false, 0));
}

// fullscreen has to hold before vrr follows it, on and off
static void test_vrr_debounce(void) {
	struct sync s;
	sync_init(&s);
	CHECK(sync_vrr_settles_at(&s) == 0);
	CHECK(!sync_vrr_wanted(&s, SYNC_VRR_FULLSCREEN, true, true, 0));
	CHECK(sync_vrr_settles_at(&s) == SYNC_VRR_DEBOUNCE_NS); // wake then to switch
	CHECK(!sync_vrr_wanted(&s, SYNC_VRR_FULLSCREEN, true, true, SYNC_VRR_DEBOUNCE_NS - 1));
	CHECK(sync_vrr_wanted(&s, SYNC_VRR_FULLSCREEN, true, true, SYNC_VRR_DEBOUNCE_NS));
	CHECK(sync_vrr_settles_at(&s) == 0);
	// out for a moment and back: stays on
	int64_t t = SYNC_VRR_DEBOUNCE_NS + 100 * MS;
	CHECK(sync_vrr_wanted(&s, SYNC_VRR_FULLSCREEN, true, false, t));
	CHECK(sync_vrr_wanted(&s, SYNC_VRR_FULLSCREEN, true, true, t + 100 * MS));
	CHECK(sync_vrr_wanted(&s, SYNC_VRR_FULLSCREEN, true, true, t + SYNC_VRR_DEBOUNCE_NS));
	// out for a second (a launcher handing over to the game): still on
	t += SYNC_VRR_DEBOUNCE_NS + 100 * MS;
	CHECK(sync_vrr_wanted(&s, SYNC_VRR_FULLSCREEN, true, false, t));
	CHECK(sync_vrr_wanted(&s, SYNC_VRR_FULLSCREEN, true, false, t + 1000 * MS));
	CHECK(sync_vrr_settles_at(&s) == t + SYNC_VRR_OFF_DEBOUNCE_NS);
	// out for good: off once the off debounce passes
	CHECK(!sync_vrr_wanted(&s, SYNC_VRR_FULLSCREEN, true, false, t + SYNC_VRR_OFF_DEBOUNCE_NS));
}

// a display that won't switch (nested backends can't turn it off): it's left
// as it is, no frame asks again
static void test_vrr_stuck(void) {
	struct sync s;
	sync_init(&s);
	CHECK(sync_vrr_switch(&s, false, true));
	sync_vrr_refused(&s);
	CHECK(!sync_vrr_switch(&s, false, true));
	CHECK(!sync_vrr_switch(&s, true, false));
	sync_reset(&s);
	CHECK(sync_vrr_switch(&s, false, true));
	CHECK(!sync_vrr_switch(&s, true, true));
}

static void test_vrr_parse(void) {
	enum sync_vrr_policy policy;
	CHECK(sync_vrr_policy_parse(NULL, &policy) && policy == SYNC_VRR_FULLSCREEN);
	CHECK(sync_vrr_policy_parse("off", &policy) && policy == SYNC_VRR_OFF);
	CHECK(sync_vrr_policy_parse("always", &policy) && policy == SYNC_VRR_ALWAYS);
	CHECK(sync_vrr_policy_parse("fullscreen", &policy) && policy == SYNC_VRR_FULLSCREEN);
	CHECK(!sync_vrr_policy_parse("on", &policy));
	CHECK(!sync_vrr_policy_parse("", &policy));
}

int main(void) {
	test_tearing_wanted();
	test_tearing_refusals();
	test_vrr_policy();
	test_vrr_debounce();
	test_vrr_stuck();
	test_vrr_parse();
	if (failures) {
		fprintf(stderr, "%d failed\n", failures);
		return 1;
	}
	printf("sync: all good\n");
	return 0;
}
