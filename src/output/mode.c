#include "output/mode.h"

static int64_t area(const struct mode_info *mode) {
	return (int64_t)mode->width * mode->height;
}

int mode_pick(const struct mode_info *modes, size_t count) {
	// the size first: preferred, or the biggest there is
	int size = -1;
	for (size_t i = 0; i < count; i++) {
		if (modes[i].preferred) {
			size = (int)i;
			break;
		}
		if (size < 0 || area(&modes[i]) > area(&modes[size])) {
			size = (int)i;
		}
	}
	if (size < 0) {
		return -1;
	}
	int best = size;
	for (size_t i = 0; i < count; i++) {
		if (modes[i].width == modes[size].width && modes[i].height == modes[size].height
				&& modes[i].refresh > modes[best].refresh) {
			best = (int)i;
		}
	}
	return best;
}

int mode_pick_asked(const struct mode_info *modes, size_t count, int32_t width,
		int32_t height, int32_t refresh) {
	int best = -1;
	int32_t best_off = 0;
	for (size_t i = 0; i < count; i++) {
		if (modes[i].width != width || modes[i].height != height) {
			continue;
		}
		if (!refresh) {
			if (best < 0 || modes[i].refresh > modes[best].refresh) {
				best = (int)i;
			}
			continue;
		}
		int32_t off = modes[i].refresh > refresh ? modes[i].refresh - refresh
			: refresh - modes[i].refresh;
		if (off <= 1000 && (best < 0 || off < best_off)) {
			best = (int)i;
			best_off = off;
		}
	}
	return best;
}
