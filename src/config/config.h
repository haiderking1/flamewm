#ifndef FLAME_CONFIG_CONFIG_H
#define FLAME_CONFIG_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bindings/keybind.h"

// read only after startup. compiled in defaults for now, a lua loader can
// build one of these at runtime later without anything else changing
struct flame_config {
	int border_width;
	int gap;

	float background_color[4];
	float border_color[4];
	float border_focused_color[4];

	int32_t repeat_rate; // keys per second
	int32_t repeat_delay; // ms

	// hold this and left drag a window onto another to swap them, or right
	// drag to resize it in place
	uint32_t drag_modifier;

	// the window under the mouse gets the keyboard as soon as you move onto it
	bool focus_follows_mouse;

	const char *cursor_theme; // NULL = default theme
	uint32_t cursor_size;

	const struct flame_keybind *keybinds;
	size_t keybind_count;
};

extern const struct flame_config flame_default_config;

#endif
