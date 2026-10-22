#include "game/pacer.h"

#include <string.h>

#define MS 1000000LL

// slower than 20 fps there's nothing left to smooth
#define SLOWEST (50 * MS)
// room on top of what the game takes, so a frame a bit slower than most
// still makes its slot
#define HEADROOM_PERCENT 8
// a frame taking this many paces is a hitch (a shader compiling), not how
// heavy the scene is: it doesn't slow the pace
#define HITCH_PACES 3
// the share of frames the pace has to fit, and the lead the release counts on
#define PERCENTILE 90

void pacer_init(struct pacer *pacer, int64_t fastest, int64_t fixed) {
	memset(pacer, 0, sizeof(*pacer));
	pacer_set_limits(pacer, fastest, fixed);
	pacer->interval = pacer->fixed > pacer->fastest ? pacer->fixed : pacer->fastest;
}

void pacer_set_limits(struct pacer *pacer, int64_t fastest, int64_t fixed) {
	pacer->fastest = fastest > 0 ? fastest : 1;
	pacer->slowest = SLOWEST > pacer->fastest ? SLOWEST : pacer->fastest;
	pacer->fixed = fixed > 0 ? fixed : 0;
}

void pacer_released(struct pacer *pacer, int64_t t) {
	pacer->released = t;
}

// the value PERCENTILE percent of the samples are at or under
static int64_t percentile(const int64_t *samples, int len) {
	int64_t sorted[PACER_SAMPLES];
	memcpy(sorted, samples, (size_t)len * sizeof(*sorted));
	for (int i = 1; i < len; i++) {
		int64_t v = sorted[i];
		int j = i;
		for (; j > 0 && sorted[j - 1] > v; j--) {
			sorted[j] = sorted[j - 1];
		}
		sorted[j] = v;
	}
	int index = (len * PERCENTILE + 99) / 100 - 1;
	return sorted[index < 0 ? 0 : index];
}

static int64_t clamp(int64_t v, int64_t lo, int64_t hi) {
	return v < lo ? lo : v > hi ? hi : v;
}

// the pace the game can hold right now, a cap from the config on top
static int64_t wanted(const struct pacer *pacer) {
	int64_t want = pacer->len ? percentile(pacer->cost, pacer->len) : pacer->fastest;
	want += want * HEADROOM_PERCENT / 100;
	if (want < pacer->fixed) {
		want = pacer->fixed;
	}
	return clamp(want, pacer->fastest, pacer->slowest);
}

void pacer_frame(struct pacer *pacer, int64_t presented, int64_t ready) {
	if (ready < presented) {
		ready = presented;
	}
	int64_t since = pacer->last_ready > presented ? pacer->last_ready : presented;
	int64_t gpu = ready - since;
	int64_t cpu = pacer->released && presented > pacer->released
		? presented - pacer->released : 0;
	int64_t cost = cpu > gpu ? cpu : gpu;
	int64_t lead = pacer->released && ready > pacer->released ? ready - pacer->released : 0;
	pacer->last_ready = ready;
	pacer->released = 0;

	if (cost < HITCH_PACES * pacer->interval) {
		pacer->cost[pacer->at] = cost;
		pacer->lead[pacer->at] = lead;
		pacer->at = (pacer->at + 1) % PACER_SAMPLES;
		if (pacer->len < PACER_SAMPLES) {
			pacer->len++;
		}
	}

	// glide: back off quick (a slot missed shows), speed up slowly (a
	// steady rise is invisible with the display following the frames)
	int64_t want = wanted(pacer);
	if (want > pacer->interval) {
		pacer->interval += (want - pacer->interval + 3) / 4;
	} else {
		pacer->interval -= (pacer->interval - want) / 48;
	}
	pacer->interval = clamp(pacer->interval, pacer->fastest, pacer->slowest);
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
	int64_t lead = 0;
	if (pacer->len) {
		int64_t leads[PACER_SAMPLES];
		int n = 0;
		for (int i = 0; i < pacer->len; i++) {
			if (pacer->lead[i] > 0) {
				leads[n++] = pacer->lead[i];
			}
		}
		lead = n ? percentile(leads, n) : 0;
	}
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
