#include "render/sync.h"

#include <string.h>

void sync_init(struct sync *sync) {
	memset(sync, 0, sizeof(*sync));
}

void sync_reset(struct sync *sync) {
	sync->refused = 0;
	sync->tearing_off = false;
	sync->vrr_off = false;
}

bool sync_tearing_wanted(const struct sync *sync, bool allowed, bool app_wants,
		bool cursor_changed) {
	return allowed && app_wants && !sync->tearing_off && !cursor_changed;
}

void sync_tearing_result(struct sync *sync, bool refused) {
	if (!refused) {
		sync->refused = 0;
		return;
	}
	if (++sync->refused >= SYNC_TEARING_REFUSALS) {
		sync->tearing_off = true;
	}
}

// how long the fullscreen it's seen has to hold: quick on, slow off
static int64_t debounce(const struct sync *sync) {
	return sync->vrr_seen ? SYNC_VRR_DEBOUNCE_NS : SYNC_VRR_OFF_DEBOUNCE_NS;
}

bool sync_vrr_wanted(struct sync *sync, enum sync_vrr_policy policy, bool supported,
		bool fullscreen, int64_t now) {
	// the debounce follows fullscreen whatever the policy, so switching the
	// policy on mid game doesn't wait for it
	if (fullscreen != sync->vrr_seen) {
		sync->vrr_seen = fullscreen;
		sync->vrr_since = now;
	}
	if (sync->vrr_seen != sync->vrr_fullscreen && now - sync->vrr_since >= debounce(sync)) {
		sync->vrr_fullscreen = sync->vrr_seen;
	}
	if (!supported || sync->vrr_off) {
		return false;
	}
	switch (policy) {
	case SYNC_VRR_OFF:
		return false;
	case SYNC_VRR_FULLSCREEN:
		return sync->vrr_fullscreen;
	case SYNC_VRR_ALWAYS:
		return true;
	}
	return false;
}

void sync_vrr_refused(struct sync *sync) {
	sync->vrr_off = true;
}

bool sync_vrr_switch(const struct sync *sync, bool wanted, bool enabled) {
	return !sync->vrr_off && wanted != enabled;
}

int64_t sync_vrr_settles_at(const struct sync *sync) {
	return sync->vrr_seen != sync->vrr_fullscreen ? sync->vrr_since + debounce(sync) : 0;
}

bool sync_vrr_policy_parse(const char *value, enum sync_vrr_policy *policy) {
	static const char *const names[] = {
		[SYNC_VRR_OFF] = "off",
		[SYNC_VRR_FULLSCREEN] = "fullscreen",
		[SYNC_VRR_ALWAYS] = "always",
	};
	if (!value) {
		*policy = SYNC_VRR_FULLSCREEN;
		return true;
	}
	for (int i = 0; i < 3; i++) {
		if (!strcmp(value, names[i])) {
			*policy = (enum sync_vrr_policy)i;
			return true;
		}
	}
	return false;
}
