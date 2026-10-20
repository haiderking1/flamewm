#ifndef FLAME_CONFIG_CONFIG_H
#define FLAME_CONFIG_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bindings/keybind.h"

struct flame_lua;
struct flame_server;

// what the keymap gets built from. NULL fields: XKB_DEFAULT_* from the
// environment, like xkbcommon does on its own
struct flame_keyboard_names {
	char *rules, *model, *layout, *variant, *options;
};

// read only while it's in use. the compiled in defaults, with the user's
// init.lua on top (config/lua.c): a reload builds a new one and swaps it in
struct flame_config {
	int border_width;
	int gap;

	float background_color[4];
	float border_color[4];
	float border_focused_color[4];

	int32_t repeat_rate; // keys per second
	int32_t repeat_delay; // ms
	struct flame_keyboard_names keyboard;

	// hold this and left drag a window onto another to swap them, or right
	// drag to resize it in place
	uint32_t drag_modifier;

	// the window under the mouse gets the keyboard as soon as you move onto it
	bool focus_follows_mouse;

	const char *cursor_theme; // NULL = default theme
	uint32_t cursor_size;

	const struct flame_keybind *keybinds;
	size_t keybind_count;

	// what config_load made, freed with it. lua: the loaded init.lua, its
	// keybinds and rules call into it. NULL for the plain defaults
	struct flame_lua *lua;
	struct flame_keybind *owned_keybinds;
	char *owned_cursor_theme;
	// commands to run once flame is up, the first load only
	char **autostart;
	size_t autostart_count;
};

extern const struct flame_config flame_default_config;

// the config to run with: the defaults, then path (or
// $XDG_CONFIG_HOME/flame/init.lua when NULL) on top. a missing or broken file
// leaves the defaults, error says what went wrong (free it). never NULL
// unless out of memory
struct flame_config *config_load(struct flame_server *server, const char *path, char **error);
void config_free(struct flame_config *config);
// where init.lua is looked for, for messages
const char *config_default_path(void);

// what the rules say about a window that just opened
struct flame_window_rule {
	bool floating_set, floating;
	bool fullscreen;
	int workspace; // 0 for wherever it would go, else 1..10
};
void config_window_rules(const struct flame_config *config, const char *app_id,
	const char *title, struct flame_window_rule *out);

// a keybind's lua function
void config_run_lua_bind(struct flame_server *server, int ref);

#endif
