// pacing a game's frames is pure logic: make test
#include <stdio.h>
#include <stdlib.h>

#include "game/pacer.h"

#define MS 1000000LL

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

// a game the pacer runs: it takes cpu ms from being let go to presenting,
// then gpu ms to be ready. slot by slot, like xshell does it. returns when
// the last frame went on screen, intervals[] gets the time between frames
struct game {
	int64_t now;
	int64_t gpu_free; // its gpu is busy till then
};

static int64_t frame(struct pacer *p, struct game *g, int64_t cpu, int64_t gpu) {
	int64_t release = pacer_release_at(p);
	bool held = release > g->now;
	if (held) {
		g->now = release;
	}
	pacer_released(p, g->now, held);
	int64_t presented = g->now + cpu;
	int64_t start = presented > g->gpu_free ? presented : g->gpu_free;
	int64_t ready = start + gpu;
	g->gpu_free = ready;
	pacer_frame(p, presented, ready);
	int64_t slot = pacer_slot(p, ready);
	pacer_shown(p, slot);
	// the game is let go again once it can be, never before its last frame showed
	g->now = presented;
	return slot;
}

// frames that take 6 to 10 ms (cpu then gpu) come out evenly spaced, at a
// pace nearly all of them make, not the fastest the display can do
static void test_uneven_becomes_flat(void) {
	struct pacer p;
	pacer_init(&p, 6 * MS, 0); // a 165Hz display
	struct game g = {0};
	srand(1);
	int64_t last = 0, sum = 0;
	int n = 0, on_pace = 0;
	for (int i = 0; i < 3000; i++) {
		int64_t cpu = 2 * MS, gpu = (4 + rand() % 5) * MS;
		int64_t slot = frame(&p, &g, cpu, gpu);
		if (i >= 1000) {
			int64_t off = slot - last - p.interval;
			on_pace += off > -MS / 2 && off < MS / 2;
			sum += slot - last;
			n++;
		}
		last = slot;
	}
	CHECK(on_pace * 100 >= n * 98); // flat
	// its slowest frames take 10 ms: the pace fits them, not much slower
	CHECK(sum / n >= 6 * MS && sum / n <= 10500000LL);
}

// a game that can easily do more than the display: the display's pace
static void test_fast_game_display_pace(void) {
	struct pacer p;
	pacer_init(&p, 6 * MS, 0);
	struct game g = {0};
	int64_t last = 0, gap = 0;
	for (int i = 0; i < 300; i++) {
		int64_t slot = frame(&p, &g, 1 * MS, 1 * MS);
		gap = slot - last;
		last = slot;
	}
	CHECK(gap == 6 * MS);
}

// a cap from the config: never faster, the game's own pace when it's slower
static void test_fixed_cap(void) {
	struct pacer p;
	pacer_init(&p, 6 * MS, 10 * MS);
	struct game g = {0};
	int64_t last = 0, gap = 0;
	for (int i = 0; i < 300; i++) {
		int64_t slot = frame(&p, &g, 1 * MS, 2 * MS);
		gap = slot - last;
		last = slot;
	}
	CHECK(gap == 10 * MS);
	for (int i = 0; i < 400; i++) {
		int64_t slot = frame(&p, &g, 1 * MS, 14 * MS);
		gap = slot - last;
		last = slot;
	}
	CHECK(gap >= 14 * MS && gap < 17 * MS); // its gpu takes 14 a frame
}

// one hitch (a shader compiling): it shows late, it doesn't slow the pace,
// and the frames after it don't rush out to catch up
static void test_hitch(void) {
	struct pacer p;
	pacer_init(&p, 6 * MS, 0);
	struct game g = {0};
	int64_t last = 0;
	for (int i = 0; i < 300; i++) {
		last = frame(&p, &g, 2 * MS, 6 * MS);
	}
	int64_t pace = p.interval;
	int64_t hitch = frame(&p, &g, 200 * MS, 6 * MS);
	CHECK(hitch - last > 200 * MS);
	last = hitch;
	for (int i = 0; i < 5; i++) {
		int64_t slot = frame(&p, &g, 2 * MS, 6 * MS);
		CHECK(slot - last >= pace - pace / 50); // no burst
		last = slot;
	}
	CHECK(p.interval <= pace + pace / 20);
}

// heavier for good: the pace backs off within a few frames, lighter again it
// comes back up slowly
static void test_glide(void) {
	struct pacer p;
	pacer_init(&p, 6 * MS, 0);
	struct game g = {0};
	for (int i = 0; i < 300; i++) {
		frame(&p, &g, 1 * MS, 7 * MS);
	}
	int64_t light = p.interval;
	for (int i = 0; i < 120; i++) {
		frame(&p, &g, 1 * MS, 14 * MS);
	}
	CHECK(p.interval > 13 * MS); // its gpu takes 14 a frame
	int64_t heavy = p.interval;
	for (int i = 0; i < 10; i++) {
		frame(&p, &g, 1 * MS, 7 * MS);
	}
	int64_t step = heavy - p.interval;
	CHECK(step < 2 * MS); // no jump back
	for (int i = 0; i < 800; i++) {
		frame(&p, &g, 1 * MS, 7 * MS);
	}
	CHECK(p.interval < light + MS);
}

// let go just in time: the frame is ready a little before its slot, not a
// whole frame early (that's lag)
static void test_release_just_in_time(void) {
	struct pacer p;
	pacer_init(&p, 6 * MS, 12 * MS);
	struct game g = {0};
	for (int i = 0; i < 300; i++) {
		frame(&p, &g, 2 * MS, 3 * MS);
	}
	int64_t release = pacer_release_at(&p);
	int64_t slot = p.last_shown + p.interval;
	int64_t ready = release + 5 * MS;
	CHECK(ready <= slot);
	CHECK(slot - ready < 3 * MS);
}

static void test_fps(void) {
	struct pacer p;
	pacer_init(&p, 10 * MS, 0);
	CHECK(pacer_fps(&p) > 99.9 && pacer_fps(&p) < 100.1);
	CHECK(pacer_release_at(&p) == 0); // nothing shown yet: right away
	CHECK(pacer_slot(&p, 5 * MS) == 5 * MS);
}

static void test_mode_parse(void) {
	enum pacer_mode mode;
	CHECK(pacer_mode_parse(NULL, &mode) && mode == PACER_VRR);
	CHECK(pacer_mode_parse("off", &mode) && mode == PACER_OFF);
	CHECK(pacer_mode_parse("always", &mode) && mode == PACER_ALWAYS);
	CHECK(pacer_mode_parse("vrr", &mode) && mode == PACER_VRR);
	CHECK(!pacer_mode_parse("on", &mode));
}

int main(void) {
	test_uneven_becomes_flat();
	test_fast_game_display_pace();
	test_fixed_cap();
	test_hitch();
	test_glide();
	test_release_just_in_time();
	test_fps();
	test_mode_parse();
	if (failures) {
		fprintf(stderr, "%d failed\n", failures);
		return 1;
	}
	printf("pacer: all good\n");
	return 0;
}
