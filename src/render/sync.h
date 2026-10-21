#ifndef FLAME_RENDER_SYNC_H
#define FLAME_RENDER_SYNC_H

#include <stdbool.h>
#include <stdint.h>

// how one output's frames meet its display, beyond plain vsync: tearing (a
// fullscreen game's frame flips the moment it's ready, mid scanout) and vrr
// (the display waits for the frame instead of the frame for the display).
// pure logic: frame.c asks, and tells it what KMS said. times in ns

// a run of refused tearing flips this long and tearing is off for the
// output till its mode changes or flame comes back from a vt switch
#define SYNC_TEARING_REFUSALS 8
// fullscreen has to hold this long before vrr follows it, either way: a
// window fullscreen for a moment doesn't make the display switch modes
#define SYNC_VRR_DEBOUNCE_NS 500000000LL

enum sync_vrr_policy {
	SYNC_VRR_OFF,
	SYNC_VRR_FULLSCREEN, // while something fullscreen shows (a game, a video)
	SYNC_VRR_ALWAYS,
};

struct sync {
	int refused; // tearing flips KMS refused in a row
	bool tearing_off; // too many: off till sync_reset
	bool tearing; // the last frame tore (or would have, then was refused)

	bool vrr_off; // KMS wouldn't turn it on: off till sync_reset
	bool vrr_fullscreen; // what the debounce settled on
	bool vrr_seen; // the last frame's fullscreen, the debounce follows it
	int64_t vrr_since; // when that last changed
};

void sync_init(struct sync *sync);
// a mode change or a vt switch: KMS gets another chance at both
void sync_reset(struct sync *sync);

// should this commit tear: allowed by the config, the fullscreen app asked,
// and nothing else changes with it (an async flip can't take the cursor
// plane along)
bool sync_tearing_wanted(const struct sync *sync, bool allowed, bool app_wants,
	bool cursor_changed);
// what came of a tearing commit: refused (it went as a plain flip) or not
void sync_tearing_result(struct sync *sync, bool refused);

// should vrr be on for the frame at now: the policy, whether the output can,
// whether something fullscreen shows on it
bool sync_vrr_wanted(struct sync *sync, enum sync_vrr_policy policy, bool supported,
	bool fullscreen, int64_t now);
// KMS wouldn't turn vrr on (the commit failed, or the status says disabled)
void sync_vrr_refused(struct sync *sync);

// NULL is the default, fullscreen. false for anything else than
// off/fullscreen/always
bool sync_vrr_policy_parse(const char *value, enum sync_vrr_policy *policy);

#endif
