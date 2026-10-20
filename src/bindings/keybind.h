#ifndef FLAME_BINDINGS_KEYBIND_H
#define FLAME_BINDINGS_KEYBIND_H

#include <stdbool.h>
#include <stdint.h>
#include <xkbcommon/xkbcommon.h>

struct flame_server;

union flame_arg {
	int i;
	float f;
	const char *const *argv;
};

typedef void (*flame_action_fn)(struct flame_server *server, const union flame_arg *arg);

struct flame_keybind {
	uint32_t mods; // WLR_MODIFIER_* mask
	xkb_keysym_t sym; // unshifted, e.g. XKB_KEY_1 even with shift held
	flame_action_fn action;
	union flame_arg arg;
};

// true if a bind matched and ran, the key never reaches the client then
bool keybind_handle(struct flame_server *server, uint32_t mods, xkb_keysym_t sym);

#endif
