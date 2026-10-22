#include <wlr/types/wlr_keyboard.h>

#include "bindings/action.h"
#include "config/config.h"
#include "util/macros.h"

// super. nested inside another compositor the host will eat most of these
#define MOD WLR_MODIFIER_LOGO
#define SHIFT WLR_MODIFIER_SHIFT

#define RGB(hex) { \
	((hex) >> 16 & 0xff) / 255.0f, \
	((hex) >> 8 & 0xff) / 255.0f, \
	((hex) & 0xff) / 255.0f, \
	1.0f, \
}

#define WORKSPACE(sym, index) \
	{ MOD, sym, action_workspace, { .i = index } }, \
	{ MOD | SHIFT, sym, action_move_to_workspace, { .i = index } }

static const char *const terminal[] = { "foot", NULL };
static const char *const settings[] = { "flamesettings", NULL };
static const char *const files[] = { "flamefiles", NULL };

static const struct flame_keybind keybinds[] = {
	{ MOD, XKB_KEY_Return, action_spawn, { .argv = terminal } },
	{ MOD, XKB_KEY_q, action_spawn, { .argv = terminal } },
	{ MOD, XKB_KEY_c, action_close, { 0 } },
	{ MOD, XKB_KEY_comma, action_spawn, { .argv = settings } },
	{ MOD, XKB_KEY_e, action_spawn, { .argv = files } },
	{ MOD, XKB_KEY_j, action_focus, { .i = +1 } },
	{ MOD, XKB_KEY_k, action_focus, { .i = -1 } },
	{ MOD, XKB_KEY_f, action_fullscreen, { 0 } },
	{ MOD | SHIFT, XKB_KEY_space, action_toggle_floating, { 0 } },
	{ MOD | SHIFT, XKB_KEY_q, action_quit, { 0 } },
	{ MOD | SHIFT, XKB_KEY_r, action_reload, { 0 } },
	WORKSPACE(XKB_KEY_1, 0),
	WORKSPACE(XKB_KEY_2, 1),
	WORKSPACE(XKB_KEY_3, 2),
	WORKSPACE(XKB_KEY_4, 3),
	WORKSPACE(XKB_KEY_5, 4),
	WORKSPACE(XKB_KEY_6, 5),
	WORKSPACE(XKB_KEY_7, 6),
	WORKSPACE(XKB_KEY_8, 7),
	WORKSPACE(XKB_KEY_9, 8),
	// 0 sits after 9 on the keyboard, so it's the 10th
	WORKSPACE(XKB_KEY_0, 9),
};

const struct flame_config flame_default_config = {
	.border_width = 2,
	.gap = 6,

	.background_color = RGB(0x111111),
	.border_color = RGB(0x2a2a2a),
	.border_focused_color = RGB(0xff6a00),

	.repeat_rate = 30,
	.repeat_delay = 250,

	.drag_modifier = MOD,
	.focus_follows_mouse = true,
	.x_zero_scaling = false,
	.x_global_keys = FLAME_X_KEYS_ALWAYS,
	.allow_tearing = true,
	.vrr = SYNC_VRR_FULLSCREEN,
	.pace = PACER_VRR,

	.cursor_theme = NULL,
	.cursor_size = 24,

	.anim = {
		.enabled = true,
		.layers = true,
		.open = FLAME_ANIM_SLIDE,
		.close = FLAME_ANIM_SLIDE,
		.layer_open = FLAME_ANIM_SLIDE,
		.layer_close = FLAME_ANIM_SLIDE,
		.fade_in = true,
		.fade_out = true,
		.fade_in_from = 0.5f,
		.fade_out_from = 0.5f,
		.zoom_from = 0.4f,
		.zoom_to = 0.8f,
		.open_ms = 400,
		.close_ms = 300,
		.move_ms = 500,
		.workspace_ms = 300,
		.focus_ms = 0,
		.open_curve = {0.46, 1.0, 0.29, 0.99},
		.close_curve = {0.46, 1.0, 0.29, 0.99},
		.move_curve = {0.46, 1.0, 0.29, 0.99},
		.workspace_curve = {0.46, 1.0, 0.29, 0.99},
		.focus_curve = {0.46, 1.0, 0.29, 0.99},
		.fade_in_curve = {0.46, 1.0, 0.29, 0.99},
		.fade_out_curve = {0.5, 0.5, 0.5, 0.5},
		.workspace_vertical = false,
		.focused_opacity = 1.0f,
		.unfocused_opacity = 1.0f,
	},

	.keybinds = keybinds,
	.keybind_count = LENGTH(keybinds),
};
