#ifndef FLAME_OUTPUT_MODE_H
#define FLAME_OUTPUT_MODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// what a display offers, refresh in mHz like wlroots has it
struct mode_info {
	int32_t width, height;
	int32_t refresh;
	bool preferred;
};

// the mode to light a display up with: its native size (the one it says it
// prefers, else the biggest) at the fastest refresh that size has. a
// display's preferred mode is often 60Hz, and some only say 60 once
// freesync is on in their menu. ties keep the preferred one. -1 for none
int mode_pick(const struct mode_info *modes, size_t count);

#endif
