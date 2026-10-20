#ifndef FLAME_INPUT_KEYBOARD_H
#define FLAME_INPUT_KEYBOARD_H

#include <wayland-server-core.h>

struct flame_input;
struct wlr_input_device;

struct flame_keyboard {
	struct wl_list link; // flame_input.keyboards
	struct flame_input *input;
	struct wlr_keyboard *wlr;

	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

// frees itself when the device goes away
void keyboard_create(struct flame_input *input, struct wlr_input_device *device);
// the config changed: every keyboard gets its layout and repeat
void keyboards_apply_config(struct flame_input *input);

#endif
