#include "render/hiccup.h"

// on top of the measured cost, the stall varies a bit
#define COST_MARGIN_NS 300000LL

static uint64_t distance(uint64_t a, uint64_t b) {
	return a > b ? a - b : b - a;
}

bool hiccup_draw(struct flame_hiccup *hiccup, int64_t ns) {
	hiccup->draws++;
	if (hiccup->usual == 0) {
		hiccup->usual = ns;
		return false;
	}
	bool stall = ns > hiccup->usual + HICCUP_MIN_NS && ns > 2 * hiccup->usual;
	if (!stall) {
		hiccup->usual += (ns - hiccup->usual) / 16;
		// the one that was due didn't show: starting that draw early got it
		// past the driver's wait. the rhythm goes on from where it was due
		if (hiccup->learned && hiccup->draws > hiccup->last + hiccup->period + HICCUP_SLACK) {
			hiccup->last += hiccup->period;
		}
		return false;
	}
	// one period is enough to plan for the next: a wrong guess only starts a
	// couple of draws early, and the next stall corrects the period
	if (hiccup->last) {
		hiccup->period = hiccup->draws - hiccup->last;
		hiccup->learned = true;
	}
	hiccup->last = hiccup->draws;
	hiccup->cost = ns - hiccup->usual;
	return true;
}

int64_t hiccup_next(const struct flame_hiccup *hiccup) {
	if (!hiccup->learned) {
		return 0;
	}
	uint64_t due = hiccup->last + hiccup->period;
	if (distance(hiccup->draws + 1, due) > 1) {
		return 0;
	}
	return hiccup->cost + COST_MARGIN_NS;
}
