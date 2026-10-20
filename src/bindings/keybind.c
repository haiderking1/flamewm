#include "bindings/keybind.h"

#include <wlr/types/wlr_keyboard.h>

#include "config/config.h"
#include "core/server.h"

bool keybind_handle(struct flame_server *server, uint32_t mods, xkb_keysym_t sym) {
	const struct flame_config *config = server->config;

	// caps lock and num lock shouldn't break binds
	mods &= ~(WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2);
	sym = xkb_keysym_to_lower(sym);

	for (size_t i = 0; i < config->keybind_count; i++) {
		const struct flame_keybind *bind = &config->keybinds[i];
		if (bind->mods == mods && bind->sym == sym) {
			bind->action(server, &bind->arg);
			return true;
		}
	}
	return false;
}
