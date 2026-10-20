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

static const struct flame_keybind keybinds[] = {
	{ MOD, XKB_KEY_Return, action_spawn, { .argv = terminal } },
	{ MOD, XKB_KEY_q, action_spawn, { .argv = terminal } },
	{ MOD, XKB_KEY_c, action_close, { 0 } },
	{ MOD, XKB_KEY_j, action_focus, { .i = +1 } },
	{ MOD, XKB_KEY_k, action_focus, { .i = -1 } },
	{ MOD, XKB_KEY_f, action_fullscreen, { 0 } },
	{ MOD | SHIFT, XKB_KEY_q, action_quit, { 0 } },
	WORKSPACE(XKB_KEY_1, 0),
	WORKSPACE(XKB_KEY_2, 1),
	WORKSPACE(XKB_KEY_3, 2),
	WORKSPACE(XKB_KEY_4, 3),
	WORKSPACE(XKB_KEY_5, 4),
	WORKSPACE(XKB_KEY_6, 5),
	WORKSPACE(XKB_KEY_7, 6),
	WORKSPACE(XKB_KEY_8, 7),
	WORKSPACE(XKB_KEY_9, 8),
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

	.cursor_theme = NULL,
	.cursor_size = 24,

	.keybinds = keybinds,
	.keybind_count = LENGTH(keybinds),
};
