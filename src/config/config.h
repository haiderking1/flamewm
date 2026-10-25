#ifndef FLAME_CONFIG_CONFIG_H
#define FLAME_CONFIG_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "anim/anim.h"
#include "bindings/keybind.h"
#include "game/pacer.h"
#include "render/sync.h"

struct flame_lua;
struct flame_server;

// what the keymap gets built from. NULL fields: XKB_DEFAULT_* from the
// environment, like xkbcommon does on its own
struct flame_keyboard_names {
	char *rules, *model, *layout, *variant, *options;
};

enum flame_anim_type {
	FLAME_ANIM_NONE,
	FLAME_ANIM_SLIDE, // in from the nearest screen edge, out to it
	FLAME_ANIM_ZOOM, // grows from small, shrinks away
	FLAME_ANIM_FADE,
};

// windows (and bars, notifications) moving into place
struct flame_anim_config {
	bool enabled; // windows: opening, closing, moving, workspaces switching
	bool layers; // bars, notifications, launchers opening and closing
	enum flame_anim_type open, close, layer_open, layer_close;
	// fading on top of a slide or zoom, from (or to) this opacity
	bool fade_in, fade_out;
	float fade_in_from, fade_out_from;
	float zoom_from, zoom_to; // a zoom opens from this size, closes to that
	// ms
	int open_ms, close_ms, move_ms, workspace_ms, focus_ms;
	struct flame_curve open_curve, close_curve, move_curve, workspace_curve, focus_curve,
		fade_in_curve, fade_out_curve;
	bool workspace_vertical; // workspaces slide up and down, not sideways
	// windows without focus fade to this, 1 for not at all
	float focused_opacity, unfocused_opacity;
};

// a screen by its connector name (DP-1, HDMI-A-1), "*" for any without
// its own
struct flame_output_config {
	char *name;
	float scale; // 0 for the default
};

// read only while it's in use. the compiled in defaults, with the user's
// init.lua on top (config/lua.c): a reload builds a new one and swaps it in
// which keys typed in wayland windows X apps see, like Xorg shows them every
// key: push to talk (discord, mumble), obs hotkeys, hotkey daemons
enum flame_x_global_keys {
	FLAME_X_KEYS_NEVER,
	FLAME_X_KEYS_MODIFIERS, // shift, ctrl, alt, super on their own
	FLAME_X_KEYS_COMBOS, // those, and any key while ctrl, alt or super is held
	FLAME_X_KEYS_ALWAYS, // every key, mouse buttons too
};

enum flame_wallpaper_mode {
	FLAME_WALLPAPER_FILL, // covers the screen, cut to it
	FLAME_WALLPAPER_FIT, // all of it shows, the background color around it
	FLAME_WALLPAPER_CENTER, // its own size, in the middle
	FLAME_WALLPAPER_STRETCH, // the screen's shape, whatever its own
};

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

	// X apps at the screen's scale draw at its pixels and are told its dpi
	// (they scale themselves, sharp), not stretched up (blurry). apps that
	// ignore dpi come out small
	bool x_zero_scaling;

	enum flame_x_global_keys x_global_keys;

	// a fullscreen app that asks to tear (a game with vsync off) does: its
	// frames go out the moment they're done, mid refresh
	bool allow_tearing;
	// the display waits for frames instead of frames for the display, where
	// it can: off, fullscreen (while a window is fullscreen, shown or not), always
	enum sync_vrr_policy vrr;
	// the fullscreen game's frames at an even pace, the fastest it holds
	// (game/pacer.h): off, vrr (while the display runs it), always. pace_fps
	// caps it, 0 for none
	enum pacer_mode pace;
	int pace_fps;

	// a picture on every screen over the background color, under everything
	// (layer-shell wallpapers too), NULL for none. how it fits a screen
	char *wallpaper;
	enum flame_wallpaper_mode wallpaper_mode;

	// what locks the screen (super+l, before the computer sleeps): a shell
	// command. NULL for flamelock, "" for nothing
	char *lock;

	const char *cursor_theme; // NULL = default theme
	uint32_t cursor_size;

	struct flame_anim_config anim;
	// glass and what's see-through over it
	struct {
		// grain (of full, +-) over glass and blended windows: dark blur
		// doesn't come out in bands
		float noise;
		// how far apps' own blur reaches (ext-background-effect: foot's),
		// layout pixels
		float size;
	} blur;

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

	struct flame_output_config *outputs;
	size_t output_count;

	// what init.lua set ("gap", "keyboard.layout", "output.DP-1.scale"):
	// settings.lua can't change those, flamesettings shows them locked
	char **pinned;
	size_t pinned_count;
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
// settings.lua: what flamesettings (flamectl set) saves, loaded before
// init.lua. $FLAME_SETTINGS, else next to init.lua
const char *config_settings_path(void);

// what the rules say about a window that just opened
struct flame_window_rule {
	bool floating_set, floating;
	bool fullscreen;
	int workspace; // 0 for wherever it would go, else 1..10
	bool no_animation;
	// -1 for the config's
	int open, close;
	bool no_fade_in, no_fade_out;
	float opacity; // 0 for the config's
};
void config_window_rules(const struct flame_config *config, const char *app_id,
	const char *title, struct flame_window_rule *out);

// what the config says about the output called name, NULL for nothing
const struct flame_output_config *config_output(const struct flame_config *config,
	const char *name);

// a keybind's lua function
void config_run_lua_bind(struct flame_server *server, int ref);

// the command that locks the screen, NULL when nothing does
static inline const char *config_lock_command(const struct flame_config *config) {
	return !config->lock ? "flamelock" : *config->lock ? config->lock : NULL;
}

#endif
