#ifndef FLAME_INPUT_INPUT_H
#define FLAME_INPUT_INPUT_H

#include <stdbool.h>
#include <wayland-server-core.h>

#include "input/cursor.h"
#include "input/drag.h"

struct flame_server;
struct wlr_surface;

struct flame_input {
	struct flame_server *server;
	struct wlr_seat *seat;
	struct flame_cursor cursor;
	struct flame_drag drag;
	struct wl_list keyboards; // flame_keyboard.link

	struct wl_listener new_input;
	struct wl_listener request_set_selection;
	struct wl_listener request_set_primary_selection;
};

bool input_init(struct flame_input *input, struct flame_server *server);
void input_finish(struct flame_input *input);

void input_update_capabilities(struct flame_input *input);
// give the keyboard to surface, NULL takes it from everyone
void input_keyboard_enter(struct flame_input *input, struct wlr_surface *surface);

#endif
