#ifndef FLAME_INPUT_CURSOR_H
#define FLAME_INPUT_CURSOR_H

#include <stdbool.h>
#include <wayland-server-core.h>

#include "cursor/image.h"

struct flame_server;
struct wlr_input_device;

struct flame_cursor {
	struct flame_server *server;
	// position and input devices. it never gets an image, so wlroots never
	// draws a cursor or blocks direct scanout over one, image is ours
	struct wlr_cursor *wlr;
	struct flame_cursor_image image;
	bool image_ready;
	struct wlr_cursor_shape_manager_v1 *shape_manager;
	// the window the mouse itself last moved into, hyprland's
	// m_lastMouseFocus: focus follows the mouse into another one, not around
	// this. windows moving under a still pointer don't change it
	struct flame_view *mouse_focused;

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
// view's going: the pointer isn't over it any more
void cursor_forget_view(struct flame_cursor *cursor, struct flame_view *view);
// an app moved the pointer to a layout point (an X client's WarpPointer): no
// mouse moved, so focus doesn't follow it
void cursor_warp(struct flame_cursor *cursor, double x, double y);
// input that didn't come from a device, for tests (flamectl pointer with
// FLAME_TEST_INPUT=1): to a layout point, a button (linux code)
void cursor_test_move(struct flame_cursor *cursor, double x, double y);
// the mouse moved by dx, dy, the way a real one moves (games hear it raw)
void cursor_test_nudge(struct flame_cursor *cursor, double dx, double dy);
void cursor_test_button(struct flame_cursor *cursor, uint32_t button, bool pressed);
// a vertical wheel scroll, 15 a notch
void cursor_test_scroll(struct flame_cursor *cursor, double delta);
// a theme cursor by name, "default", "grabbing", "nw-resize"...
void cursor_set_name(struct flame_cursor *cursor, const char *name);
// an X client's cursor: frames of pixels at scale, the same key is the same
// look (see cursor_image_set_pixels)
void cursor_set_pixels(struct flame_cursor *cursor, uint64_t key,
	const struct flame_cursor_pixels *frames, size_t len, float scale);
// no pointer at all, an app hid it
void cursor_hide(struct flame_cursor *cursor);

#endif
