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
// a frame quicker than the lead the release counts on brings it down by
// this fraction of the difference. a slower one raises it right away: a
// release too late makes the frame late, and that's flame's doing, not the
// scene's
#define LEAD_DOWN_DIV 32
// late, the pace backs off at most this fraction of itself in one frame
#define BACK_OFF_MAX_DIV 10

// the fastest the pace may be: the display's, or the cap
static int64_t fastest_pace(const struct pacer *pacer) {
	return pacer->fixed > pacer->fastest ? pacer->fixed : pacer->fastest;
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

void pacer_released(struct pacer *pacer, int64_t t, bool held) {
	pacer->released = t;
	pacer->held = held;
}

static int64_t clamp(int64_t v, int64_t lo, int64_t hi) {
	return v < lo ? lo : v > hi ? hi : v;
}

// the pace comes from how frames turn out, not from what the game does
// inside: its present can come from a thread of its own (dxgi on vulkan)
// that says nothing about the game. busy: what the frame took once it
// could start (let go, and the frame before it done): longer than the pace,
// the game can't hold it and the pace backs off toward it. late with time
// to spare, flame let it go too late: the lead grows, the pace stays. on
// time, the pace speeds up a hair. it settles where nearly every frame
// makes its slot
void pacer_frame(struct pacer *pacer, int64_t presented, int64_t ready) {
	if (ready < presented) {
		ready = presented;
	}
	int64_t start = pacer->released > pacer->last_ready ? pacer->released : pacer->last_ready;
	int64_t busy = start ? ready - start : 0;
	int64_t lead = pacer->released && ready > pacer->released ? ready - pacer->released : 0;
	bool held = pacer->held;
	pacer->released = 0;
	pacer->held = false;
	pacer->last_ready = ready;
	if (lead > 0 && lead < HITCH_PACES * pacer->interval) {
		if (lead > pacer->lead) {
			pacer->lead = lead;
		} else {
			pacer->lead -= (pacer->lead - lead) / LEAD_DOWN_DIV;
		}
	}
	if (!pacer->last_shown) {
		return;
	}
	int64_t late = ready - (pacer->last_shown + pacer->interval);
	if (late > HITCH_PACES * pacer->interval || busy > HITCH_PACES * pacer->interval) {
		return; // a hitch: nothing to learn about the scene
	}
	if (busy > pacer->interval + LATE_NS) {
		int64_t back = (busy - pacer->interval) / 2 + pacer->interval / 50;
		int64_t most = pacer->interval / BACK_OFF_MAX_DIV;
		pacer->interval += back < most ? back : most;
	} else if (late > LATE_NS && held) {
		// let go too late: the lead took it, the pace stays
	} else if (late <= LATE_NS) {
		pacer->interval -= pacer->interval / SPEED_UP_DIV;
	}
	pacer->interval = clamp(pacer->interval, fastest_pace(pacer), pacer->slowest);
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
	int64_t lead = pacer->lead;
	int64_t margin = pacer->interval / 10 > MS ? pacer->interval / 10 : MS;
	return pacer->last_shown + pacer->interval - lead - margin;
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
