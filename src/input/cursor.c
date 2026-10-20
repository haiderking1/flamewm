#include "input/cursor.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

#include "config/config.h"
#include "core/server.h"
#include "util/listener.h"

static uint32_t now_msec(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

// the mouse moved onto something, same rules as hyprland's follow_mouse = 1.
// view is the window under it, else surface may be a layer (bar, launcher).
// the background leaves focus where it is
static void follow_mouse(struct flame_server *server, struct flame_view *view,
		struct wlr_surface *surface) {
	struct flame_desktop *desktop = &server->desktop;
	if (!server->config->focus_follows_mouse) {
		return;
	}
	// selecting text and dragging out of the window keeps typing where it was
	if (server->input.seat->pointer_state.button_count > 0) {
		return;
	}
	if (!view) {
		// a layer that wants the keyboard on demand takes it on hover too,
		// bars that never want it are left alone
		if (surface) {
			layers_offer_keyboard(&server->layers, surface);
		}
		return;
	}
	// an open launcher keeps the keyboard
	if (view->workspace != desktop->active || layers_keyboard_locked(&server->layers)) {
		return;
	}
	// already typing here. unless a layer took the keyboard on hover, then
	// coming back onto the window takes it back
	if (view == desktop->focused && !server->layers.focused) {
		return;
	}
	// nothing behind a fullscreen window gets focus by peeking out
	if (desktop->active->fullscreen && desktop->active->fullscreen != view) {
		return;
	}
	focus_view(desktop, view);
}

// moved: the mouse itself moved. a refresh (things moved under a still
// cursor) updates who gets pointer events but never moves keyboard focus,
// or a window opening next to the cursor would lose it right away
static void process_motion(struct flame_cursor *cursor, uint32_t time_msec, bool moved) {
	struct flame_server *server = cursor->server;
	struct wlr_seat *seat = server->input.seat;
	render_pointer_move(&server->render, cursor->wlr->x, cursor->wlr->y);

	if (drag_active(&server->input.drag)) {
		if (moved) {
			drag_motion(&server->input.drag);
		}
		return;
	}

	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct flame_view *view = desktop_view_at(&server->desktop,
		cursor->wlr->x, cursor->wlr->y, &surface, &sx, &sy);
	if (moved) {
		follow_mouse(server, view, surface);
	}

	if (!surface) {
		wlr_seat_pointer_clear_focus(seat);
		return;
	}
	wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
	wlr_seat_pointer_notify_motion(seat, time_msec, sx, sy);
}

static void handle_motion(struct wl_listener *listener, void *data) {
	struct flame_cursor *cursor = wl_container_of(listener, cursor, motion);
	struct wlr_pointer_motion_event *event = data;
	wlr_cursor_move(cursor->wlr, &event->pointer->base, event->delta_x, event->delta_y);
	process_motion(cursor, event->time_msec, true);
}

static void handle_motion_absolute(struct wl_listener *listener, void *data) {
	struct flame_cursor *cursor = wl_container_of(listener, cursor, motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	wlr_cursor_warp_absolute(cursor->wlr, &event->pointer->base, event->x, event->y);
	process_motion(cursor, event->time_msec, true);
}

static void handle_button(struct wl_listener *listener, void *data) {
	struct flame_cursor *cursor = wl_container_of(listener, cursor, button);
	struct flame_server *server = cursor->server;
	struct wlr_pointer_button_event *event = data;

	bool pressed = event->state == WL_POINTER_BUTTON_STATE_PRESSED;
	if (drag_button(&server->input.drag, event->button, pressed)) {
		return;
	}

	if (pressed) {
		double sx, sy;
		struct wlr_surface *surface;
		struct flame_view *view = desktop_view_at(&server->desktop,
			cursor->wlr->x, cursor->wlr->y, &surface, &sx, &sy);
		if (view) {
			focus_view(&server->desktop, view);
		} else if (surface) {
			layers_offer_keyboard(&server->layers, surface);
		}
	}
	wlr_seat_pointer_notify_button(server->input.seat, event->time_msec,
		event->button, event->state);
}

static void handle_axis(struct wl_listener *listener, void *data) {
	struct flame_cursor *cursor = wl_container_of(listener, cursor, axis);
	struct wlr_pointer_axis_event *event = data;
	if (drag_active(&cursor->server->input.drag)) {
		return;
	}
	wlr_seat_pointer_notify_axis(cursor->server->input.seat, event->time_msec,
		event->orientation, event->delta, event->delta_discrete, event->source,
		event->relative_direction);
}

static void handle_frame(struct wl_listener *listener, void *data) {
	struct flame_cursor *cursor = wl_container_of(listener, cursor, frame);
	wlr_seat_pointer_notify_frame(cursor->server->input.seat);
}

static bool from_focused_client(struct flame_cursor *cursor,
		struct wlr_seat_client *client) {
	return cursor->server->input.seat->pointer_state.focused_client == client;
}

static void handle_request_set_cursor(struct wl_listener *listener, void *data) {
	struct flame_cursor *cursor = wl_container_of(listener, cursor, request_set_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	if (from_focused_client(cursor, event->seat_client)) {
		cursor_image_set_surface(&cursor->image, event->surface,
			event->hotspot_x, event->hotspot_y);
	}
}

static void handle_request_set_shape(struct wl_listener *listener, void *data) {
	struct flame_cursor *cursor = wl_container_of(listener, cursor, request_set_shape);
	struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
	if (event->device_type == WLR_CURSOR_SHAPE_MANAGER_V1_DEVICE_TYPE_POINTER
			&& from_focused_client(cursor, event->seat_client)) {
		cursor_set_name(cursor, wlr_cursor_shape_v1_name(event->shape));
	}
}

static void handle_pointer_focus_change(struct wl_listener *listener, void *data) {
	// pointer left every client, nobody's going to set an image for us
	struct flame_cursor *cursor = wl_container_of(listener, cursor, pointer_focus_change);
	struct wlr_seat_pointer_focus_change_event *event = data;
	if (!event->new_surface) {
		cursor_set_name(cursor, "default");
	}
}

bool cursor_init(struct flame_cursor *cursor, struct flame_server *server) {
	const struct flame_config *config = server->config;
	struct wlr_seat *seat = server->input.seat;

	cursor->server = server;
	cursor->wlr = wlr_cursor_create();
	cursor->shape_manager = wlr_cursor_shape_manager_v1_create(server->display, 1);
	if (!cursor->wlr || !cursor->shape_manager) {
		wlr_log(WLR_ERROR, "failed to set up the cursor");
		return false;
	}
	cursor->image_ready = cursor_image_init(&cursor->image, server->loop,
		config->cursor_theme, config->cursor_size);
	if (!cursor->image_ready) {
		return false;
	}
	wlr_cursor_attach_output_layout(cursor->wlr, server->outputs.layout);
	render_pointer_set_image(&server->render, &cursor->image);
	render_pointer_move(&server->render, cursor->wlr->x, cursor->wlr->y);

	// so clients draw their own cursors at the same size
	char size[16];
	snprintf(size, sizeof(size), "%u", config->cursor_size);
	setenv("XCURSOR_SIZE", size, true);
	if (config->cursor_theme) {
		setenv("XCURSOR_THEME", config->cursor_theme, true);
	}

	listener_connect(&cursor->motion, &cursor->wlr->events.motion, handle_motion);
	listener_connect(&cursor->motion_absolute, &cursor->wlr->events.motion_absolute,
		handle_motion_absolute);
	listener_connect(&cursor->button, &cursor->wlr->events.button, handle_button);
	listener_connect(&cursor->axis, &cursor->wlr->events.axis, handle_axis);
	listener_connect(&cursor->frame, &cursor->wlr->events.frame, handle_frame);
	listener_connect(&cursor->request_set_cursor, &seat->events.request_set_cursor,
		handle_request_set_cursor);
	listener_connect(&cursor->request_set_shape,
		&cursor->shape_manager->events.request_set_shape, handle_request_set_shape);
	listener_connect(&cursor->pointer_focus_change,
		&seat->pointer_state.events.focus_change, handle_pointer_focus_change);
	return true;
}

void cursor_finish(struct flame_cursor *cursor) {
	listener_disconnect(&cursor->motion);
	listener_disconnect(&cursor->motion_absolute);
	listener_disconnect(&cursor->button);
	listener_disconnect(&cursor->axis);
	listener_disconnect(&cursor->frame);
	listener_disconnect(&cursor->request_set_cursor);
	listener_disconnect(&cursor->request_set_shape);
	listener_disconnect(&cursor->pointer_focus_change);

	if (cursor->image_ready) {
		render_pointer_set_image(&cursor->server->render, NULL);
		cursor_image_finish(&cursor->image);
		cursor->image_ready = false;
	}
	wlr_cursor_destroy(cursor->wlr);
}

void cursor_attach_device(struct flame_cursor *cursor, struct wlr_input_device *device) {
	wlr_cursor_attach_input_device(cursor->wlr, device);
}

void cursor_refresh(struct flame_cursor *cursor) {
	process_motion(cursor, now_msec(), false);
}

void cursor_set_name(struct flame_cursor *cursor, const char *name) {
	cursor_image_set_name(&cursor->image, name);
}
