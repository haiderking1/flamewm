// learning a periodic draw stall is pure logic: make test
#include <stdio.h>

#include "render/hiccup.h"

#define US 1000LL
#define MS 1000000LL

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

// n normal draws, then one that stalls. returns whether the stall was flagged,
// and whether the draw before it was told to expect it
static bool run(struct flame_hiccup *h, int normal, int64_t *expected) {
	for (int i = 0; i < normal; i++) {
		*expected = hiccup_next(h);
		CHECK(!hiccup_draw(h, 200 * US));
	}
	*expected = hiccup_next(h);
	return hiccup_draw(h, 2300 * US);
}

static void test_learns_a_rhythm(void) {
	struct flame_hiccup h = {0};
	int64_t expected;
	CHECK(run(&h, 492, &expected));
	CHECK(expected == 0); // never seen one
	CHECK(run(&h, 492, &expected));
	CHECK(expected == 0); // the second shows the period: predictable from here
	CHECK(h.learned);
	CHECK(run(&h, 492, &expected));
	CHECK(expected > 2 * MS); // the stall's draw got its time
}

static void test_only_around_the_stall(void) {
	struct flame_hiccup h = {0};
	int64_t expected;
	for (int i = 0; i < 3; i++) {
		run(&h, 492, &expected);
	}
	// a draw right after a stall and in the middle get nothing extra
	int extra = 0;
	for (int i = 0; i < 492; i++) {
		if (hiccup_next(&h)) {
			extra++;
		}
		hiccup_draw(&h, 200 * US);
	}
	CHECK(extra >= 1 && extra <= 2); // the one before it (and maybe the one at it)
}

static void test_dodged_stall_keeps_the_rhythm(void) {
	struct flame_hiccup h = {0};
	int64_t expected;
	for (int i = 0; i < 3; i++) {
		run(&h, 492, &expected);
	}
	CHECK(h.learned);
	// planned for and it never came: the next one is still due a period on
	int planned = 0;
	for (int i = 0; i < 2 * 493; i++) {
		if (hiccup_next(&h)) {
			planned++;
		}
		CHECK(!hiccup_draw(&h, 200 * US));
	}
	CHECK(h.learned);
	CHECK(planned >= 2); // both due spots got their time
	// and when it does come back on the rhythm, it was expected
	CHECK(run(&h, 492, &expected));
	CHECK(expected > 2 * MS);
	CHECK(h.learned);
}

static void test_rhythm_change_relearns(void) {
	struct flame_hiccup h = {0};
	int64_t expected;
	for (int i = 0; i < 3; i++) {
		run(&h, 492, &expected);
	}
	// a stall way off the rhythm: the period is set from it
	CHECK(run(&h, 200, &expected));
	CHECK(expected == 0); // that one wasn't seen coming
	CHECK(h.period == 201);
	CHECK(run(&h, 200, &expected));
	CHECK(expected > 2 * MS); // the new rhythm's next one was
}

static void test_a_slow_frame_is_no_stall_when_everything_is_slow(void) {
	struct flame_hiccup h = {0};
	for (int i = 0; i < 100; i++) {
		hiccup_draw(&h, 3 * MS); // heavy scenes: 3ms is normal
	}
	CHECK(!hiccup_draw(&h, 4 * MS)); // not twice the usual
	CHECK(hiccup_draw(&h, 7 * MS)); // this one is
}

int main(void) {
	test_learns_a_rhythm();
	test_only_around_the_stall();
	test_dodged_stall_keeps_the_rhythm();
	test_rhythm_change_relearns();
	test_a_slow_frame_is_no_stall_when_everything_is_slow();
	if (failures) {
		fprintf(stderr, "%d failed\n", failures);
		return 1;
	}
	puts("hiccup: all good");
	return 0;
}
