#include "game/pacer.h"

#include <string.h>

#define MS 1000000LL

// slower than 20 fps there's nothing left to smooth
#define SLOWEST (50 * MS)
// a frame this far past its slot is late: the pace backs off
#define LATE_NS (MS / 2)
// a frame this many paces late is a hitch (a shader compiling), not how
// heavy the scene is: the pace stays
#define HITCH_PACES 3
// on time, the pace speeds up by this fraction of itself a frame: a tenth
// in about 100 frames, invisible with the display following the frames.
// faster and it settles where a share of frames are late
#define SPEED_UP_DIV 1000
// a frame ready earlier than it has to be (more than the margin before its
// slot) brings the lead down by this fraction of the difference. a later one
// raises it right away, by all it missed: a release too late makes the
// frame late, and that's flame's doing, not the scene's
#define LEAD_DOWN_DIV 32
// late, the pace backs off at most this fraction of itself in one frame
#define BACK_OFF_MAX_DIV 10
// the lead is never more than this many paces: further ahead is a game
// that doesn't wait on its release at all
#define LEAD_PACES 2

// the fastest the pace may be: the display's, or the cap
static int64_t fastest_pace(const struct pacer *pacer) {
	return pacer->fixed > pacer->fastest ? pacer->fixed : pacer->fastest;
}

// how much before its slot a frame's meant to be ready: the timer and the
// game's own jitter
static int64_t margin(const struct pacer *pacer) {
	return pacer->interval / 10 > MS ? pacer->interval / 10 : MS;
}

static int64_t clamp(int64_t v, int64_t lo, int64_t hi) {
	return v < lo ? lo : v > hi ? hi : v;
}

static int64_t clamp_pace(const struct pacer *pacer, int64_t interval) {
	return clamp(interval, fastest_pace(pacer), pacer->slowest);
}

void pacer_init(struct pacer *pacer, int64_t fastest, int64_t fixed) {
	memset(pacer, 0, sizeof(*pacer));
	pacer_set_limits(pacer, fastest, fixed);
	pacer->interval = fastest_pace(pacer);
}

void pacer_set_limits(struct pacer *pacer, int64_t fastest, int64_t fixed) {
	pacer->fastest = fastest > 0 ? fastest : 1;
	pacer->slowest = SLOWEST > pacer->fastest ? SLOWEST : pacer->fastest;
	pacer->fixed = fixed > 0 ? fixed : 0;
}

void pacer_resume(struct pacer *pacer, const struct pacer *before) {
	pacer->interval = clamp_pace(pacer, before->interval);
	pacer->lead = before->lead;
}

void pacer_released(struct pacer *pacer, int64_t t, bool held) {
	pacer->released = t;
	pacer->held = held;
}


// the pace comes from how frames turn out, not from what the game does
// inside: its present can come from a thread of its own (dxgi on vulkan)
// that says nothing about the game. busy: what the frame took once it
// could start (let go, and the frame before it done): longer than the pace,
// the game can't hold it and the pace backs off toward it. on time, the
// pace speeds up a hair. it settles where nearly every frame makes its slot.
// the lead goes by the outcome too, never by let go -> ready: a vsync game
// sends its present only once the frame before showed, that time says
// nothing about its work. ready past when it's meant to be (the margin
// before its slot) while it waited on its release, the lead grows by what
// it missed. earlier, it comes down
bool pacer_frame(struct pacer *pacer, int64_t presented, int64_t ready) {
	if (ready < presented) {
		ready = presented;
	}
	int64_t released = pacer->released;
	int64_t start = released > pacer->last_ready ? released : pacer->last_ready;
	int64_t busy = start ? ready - start : 0;
	// it waited on the release, not on its frame before: an earlier one
	// would have had it ready earlier
	bool held = pacer->held && released > pacer->last_ready;
	pacer->released = 0;
	pacer->held = false;
	pacer->last_ready = ready;
	if (!pacer->last_shown) {
		return false;
	}
	int64_t late = ready - (pacer->last_shown + pacer->interval);
	if (late > HITCH_PACES * pacer->interval || busy > HITCH_PACES * pacer->interval) {
		return false; // a hitch: nothing to learn about the scene
	}
	if (busy > pacer->interval + LATE_NS) {
		int64_t back = (busy - pacer->interval) / 2 + pacer->interval / 50;
		int64_t most = pacer->interval / BACK_OFF_MAX_DIV;
		pacer->interval += back < most ? back : most;
	} else if (late > LATE_NS && held) {
		// let go too late: the lead takes it, the pace stays
	} else if (late <= LATE_NS) {
		pacer->interval -= pacer->interval / SPEED_UP_DIV;
	}
	// past when it's meant to be ready (the margin before its slot)
	int64_t m = margin(pacer);
	int64_t past = late + m;
	if (past > 0 && held) {
		pacer->lead += past;
	} else if (past < 0) {
		pacer->lead -= -past / LEAD_DOWN_DIV;
	}
	pacer->interval = clamp_pace(pacer, pacer->interval);
	pacer->lead = clamp(pacer->lead, 0, LEAD_PACES * pacer->interval);
	// ready before its release, or earlier than any release by the lead
	// could make it: it was drawn on a buffer the game had already
	return released && (ready < released || -late > pacer->lead + m + pacer->interval / 4);
}

int64_t pacer_slot(const struct pacer *pacer, int64_t ready) {
	if (!pacer->last_shown) {
		return ready;
	}
	int64_t slot = pacer->last_shown + pacer->interval;
	if (ready >= slot) {
		return ready; // late for it (or right on it): no point holding it
	}
	return slot;
}

void pacer_shown(struct pacer *pacer, int64_t slot) {
	pacer->last_shown = slot;
}

int64_t pacer_release_at(const struct pacer *pacer) {
	if (!pacer->last_shown) {
		return 0;
	}
	return pacer->last_shown + pacer->interval - pacer->lead - margin(pacer);
}

double pacer_fps(const struct pacer *pacer) {
	return pacer->interval > 0 ? 1e9 / (double)pacer->interval : 0;
}

bool pacer_mode_parse(const char *value, enum pacer_mode *mode) {
	static const char *const names[] = {
		[PACER_OFF] = "off",
		[PACER_VRR] = "vrr",
		[PACER_ALWAYS] = "always",
	};
	if (!value) {
		*mode = PACER_VRR;
		return true;
	}
	for (int i = 0; i < 3; i++) {
		if (!strcmp(value, names[i])) {
			*mode = (enum pacer_mode)i;
			return true;
		}
	}
	return false;
}
