// which mode a display lights up with is pure logic: make test
#include <stdio.h>

#include "output/mode.h"

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

// a 165Hz display with freesync on says 60 is what it prefers
static void test_fastest_at_native(void) {
	struct mode_info modes[] = {
		{1920, 1080, 60000, true},
		{1920, 1080, 164955, false},
		{1920, 1080, 143981, false},
		{1680, 1050, 59954, false},
		{1280, 1024, 75025, false},
	};
	CHECK(mode_pick(modes, 5) == 1);
}

// a faster mode at another size doesn't win over the native one
static void test_size_first(void) {
	struct mode_info modes[] = {
		{1280, 720, 240000, false},
		{2560, 1440, 60000, true},
		{2560, 1440, 144000, false},
	};
	CHECK(mode_pick(modes, 3) == 2);
}

// nothing preferred: the biggest, at its fastest
static void test_no_preferred(void) {
	struct mode_info modes[] = {
		{1280, 720, 60000, false},
		{1920, 1080, 60000, false},
		{1920, 1080, 120000, false},
		{1024, 768, 75000, false},
	};
	CHECK(mode_pick(modes, 4) == 2);
}

// the same refresh twice keeps the preferred one
static void test_tie(void) {
	struct mode_info modes[] = {
		{1920, 1080, 60000, false},
		{1920, 1080, 60000, true},
	};
	CHECK(mode_pick(modes, 2) == 1);
}

static void test_none(void) {
	CHECK(mode_pick(NULL, 0) == -1);
}

int main(void) {
	test_fastest_at_native();
	test_size_first();
	test_no_preferred();
	test_tie();
	test_none();
	if (failures) {
		fprintf(stderr, "%d failed\n", failures);
		return 1;
	}
	printf("mode: all good\n");
	return 0;
}
