#ifndef FLAME_INPUT_CURSOR_H
#define FLAME_INPUT_CURSOR_H

#include <stdbool.h>
#include <wayland-server-core.h>

struct flame_server;
struct wlr_input_device;

struct flame_cursor {
	struct flame_server *server;
	struct wlr_cursor *wlr;
	struct wlr_xcursor_manager *xcursor;
	struct wlr_cursor_shape_manager_v1 *shape_manager;

	struct wl_listener motion;
	struct wl_listener motion_absolute;
	struct wl_listener button;
	struct wl_listener axis;
	struct wl_listener frame;
	struct wl_listener request_set_cursor;
	struct wl_listener request_set_shape;
	struct wl_listener pointer_focus_change;
};

bool cursor_init(struct flame_cursor *cursor, struct flame_server *server);
void cursor_finish(struct flame_cursor *cursor);

void cursor_attach_device(struct flame_cursor *cursor, struct wlr_input_device *device);
// redo pointer focus after windows moved under a still cursor
void cursor_refresh(struct flame_cursor *cursor);

#endif
