#ifndef FLAME_RENDER_HICCUP_H
#define FLAME_RENDER_HICCUP_H

#include <stdbool.h>
#include <stdint.h>

// a draw that stalls on a fixed rhythm, learned and planned for. nvidia's
// kernel driver sleeps ~2ms every few hundred draws (its command ring coming
// around, a 1ms sleep that takes two ticks), whatever the GL calls are. it
// can't be avoided, but it comes back after the same number of draws: once
// two have shown how far apart they are, the draw where the next one is due
// starts early enough to take it (starting early often dodges the wait
// altogether, the rhythm is kept going then), every other draw keeps the
// tight lead. one showing up somewhere else sets the rhythm from there.
// the stalls stay out of the render time estimate, they'd inflate the lead
// for every frame
struct flame_hiccup {
	uint64_t draws; // seen so far
	int64_t usual; // ns a draw normally takes, slow average of non-stalls
	uint64_t last; // draw number of the newest stall, 0 for none yet
	uint64_t period; // draws between the newest two stalls, 0 unknown
	bool learned; // a period is known: the next one is predictable
	int64_t cost; // how much longer than usual the newest stall took, ns
};

// draws a stall is longer than usual by at least, and the slack on a
// predicted one's position
#define HICCUP_MIN_NS 1000000LL
#define HICCUP_SLACK 2

// a draw took ns. true when it was a stall: keep it out of the estimates
bool hiccup_draw(struct flame_hiccup *hiccup, int64_t ns);
// the extra time the next draw needs: the stall's cost when one is due
// around it, else 0
int64_t hiccup_next(const struct flame_hiccup *hiccup);

#endif
