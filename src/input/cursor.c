#include "input/cursor.h"

#include <math.h>
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
#include "scene/surface.h"
#include "util/listener.h"

static uint32_t now_msec(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

// the pointer's over view now (NULL: a layer, surface, or the background),
// moved there or something moved under it. focus follows the mouse into
// another window, like sway and hyprland's mouse_refocus = 0: not as it
// moves around inside the one it was over, and never by itself. a window
// that opened (a game started from steam's play button) keeps the focus
// it got while the mouse wiggles where it was
static void follow_mouse(struct flame_cursor *cursor, struct flame_view *view,
		struct wlr_surface *surface, bool moved) {
	struct flame_server *server = cursor->server;
	struct flame_desktop *desktop = &server->desktop;
	// selecting text and dragging out of the window keeps typing where it
	// was, an X window's buttons too (they never reach the seat). an X
	// app's open menu keeps it too, until it closes. what it's let go over
	// it's still to cross into
	if (server->input.seat->pointer_state.button_count > 0
			|| xshell_buttons_held(&server->xshell) || xshell_menu_open(&server->xshell)) {
		return;
	}
	bool entered = view != cursor->over;
	cursor->over = view;
	if (!moved || !server->config->focus_follows_mouse || (view && !entered)) {
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

// a held button keeps the pointer on the surface it went down on, the
// implicit grab every X server and compositor has: drag a scrollbar or
// select text past the window's edge, over another window or an X one, and
// the motion and the release still go there. false when nothing holds it
static bool held_motion(struct flame_cursor *cursor, uint32_t time_msec) {
	struct wlr_seat *seat = cursor->server->input.seat;
	struct wlr_surface *held = seat->pointer_state.focused_surface;
	if (!held || seat->pointer_state.button_count == 0) {
		return false;
	}
	// where it is on screen, the last frame had it there
	struct flame_surface_state *state = scene_surface_state(held);
	if (!state || !state->drawn_seq) {
		return false;
	}
	wlr_seat_pointer_notify_motion(seat, time_msec,
		cursor->wlr->x - state->drawn_x, cursor->wlr->y - state->drawn_y);
	return true;
}

// a drag and drop: the surface under the pointer is what the drag is over,
// wlroots' drag grab tells it. nothing keeps the pointer where the button
// went down, that's the point
static void dnd_pointer(struct flame_cursor *cursor, uint32_t time_msec) {
	struct flame_server *server = cursor->server;
	struct wlr_seat *seat = server->input.seat;
	dnd_motion(&server->input.dnd);
	double sx, sy;
	struct wlr_surface *surface = NULL;
	desktop_view_at(&server->desktop, cursor->wlr->x, cursor->wlr->y, &surface, &sx, &sy);
	if (!surface) {
		// maybe an X window is what it's over, XDND then
		wlr_seat_pointer_notify_clear_focus(seat);
		xselection_dnd_over_x(&server->xselection, cursor->wlr->x, cursor->wlr->y, time_msec);
		return;
	}
	xselection_dnd_left_x(&server->xselection);
	wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
	wlr_seat_pointer_notify_motion(seat, time_msec, sx, sy);
}

// moved: the mouse itself moved. a refresh (things moved under a still
// cursor) updates who gets pointer events but never moves keyboard focus,
// or a window opening next to the cursor would lose it right away
static void process_motion(struct flame_cursor *cursor, uint32_t time_msec, bool moved) {
	struct flame_server *server = cursor->server;
	struct wlr_seat *seat = server->input.seat;
	render_pointer_move(&server->render, cursor->wlr->x, cursor->wlr->y);
	if (lock_active(&server->lock)) {
		lock_pointer(&server->lock, cursor->wlr->x, cursor->wlr->y, time_msec, false);
		return; // nothing under the locker sees it go by
	}

	if (moved) {
		xbridge_pointer_input(&server->xbridge, false);
		xselection_pointer_moved(&server->xselection);
	}
	if (drag_active(&server->input.drag)) {
		if (moved) {
			drag_motion(&server->input.drag);
		}
		return;
	}
	if (dnd_active(&server->input.dnd)) {
		dnd_pointer(cursor, time_msec);
		return;
	}
	if (picker_active(&server->picker)) {
		picker_pointer_motion(&server->picker, cursor->wlr->x, cursor->wlr->y);
		return;
	}
	if (held_motion(cursor, time_msec)) {
		return;
	}

	// an X11 window: its events go through flame's X server, not the seat
	struct flame_view *xview;
	if (xshell_pointer_motion(&server->xshell, cursor->wlr->x, cursor->wlr->y, time_msec,
			&xview)) {
		if (xview) {
			follow_mouse(cursor, xview, NULL, moved);
		}
		wlr_seat_pointer_clear_focus(seat);
		return;
	}

	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct flame_view *view = desktop_view_at(&server->desktop,
		cursor->wlr->x, cursor->wlr->y, &surface, &sx, &sy);
	follow_mouse(cursor, view, surface, moved);

	// an X11 app's menu is open: like its pointer grab, the app's other
	// windows don't see the pointer go by
	if (!surface || xbridge_menu_holds_pointer(&server->xbridge, surface)) {
		wlr_seat_pointer_clear_focus(seat);
		return;
	}
	wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
	wlr_seat_pointer_notify_motion(seat, time_msec, sx, sy);
}

// an X grab confines the pointer: it goes no further than the box's edge
static void confine(struct flame_cursor *cursor) {
	double x0, y0, x1, y1;
	if (!xshell_pointer_confine(&cursor->server->xshell, &x0, &y0, &x1, &y1)) {
		return;
	}
	double x = fmin(fmax(cursor->wlr->x, x0), x1), y = fmin(fmax(cursor->wlr->y, y0), y1);
	if (x != cursor->wlr->x || y != cursor->wlr->y) {
		wlr_cursor_warp_closest(cursor->wlr, NULL, x, y);
	}
}

// the mouse moved by dx, dy: games hear it raw (X ones, wayland ones), the
// pointer goes along unless one has it locked or confined
static void moved_by(struct flame_cursor *cursor, struct wlr_input_device *device,
		uint32_t time_msec, double dx, double dy, double unaccel_dx, double unaccel_dy) {
	bool locked = lock_active(&cursor->server->lock);
	if (!locked) {
		xshell_pointer_raw_motion(&cursor->server->xshell, dx, dy, unaccel_dx, unaccel_dy,
			time_msec);
		if (constraints_motion(&cursor->server->input.constraints, time_msec, &dx, &dy,
				unaccel_dx, unaccel_dy)) {
			return; // locked in place: the game heard how far it went, that's all
		}
	}
	wlr_cursor_move(cursor->wlr, device, dx, dy);
	if (!locked) {
		confine(cursor); // an X app's grab doesn't hold the pointer on the lock
	}
	process_motion(cursor, time_msec, true);
}

static void handle_motion(struct wl_listener *listener, void *data) {
	struct flame_cursor *cursor = wl_container_of(listener, cursor, motion);
	struct wlr_pointer_motion_event *event = data;
	moved_by(cursor, &event->pointer->base, event->time_msec, event->delta_x, event->delta_y,
		event->unaccel_dx, event->unaccel_dy);
}

static void handle_motion_absolute(struct wl_listener *listener, void *data) {
	struct flame_cursor *cursor = wl_container_of(listener, cursor, motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	wlr_cursor_warp_absolute(cursor->wlr, &event->pointer->base, event->x, event->y);
	if (!lock_active(&cursor->server->lock)) {
		confine(cursor);
	}
	process_motion(cursor, event->time_msec, true);
}

static void button_event(struct flame_cursor *cursor,
		const struct wlr_pointer_button_event *event) {
	struct flame_server *server = cursor->server;

	bool pressed = event->state == WL_POINTER_BUTTON_STATE_PRESSED;
	if (lock_active(&server->lock)) {
		lock_pointer(&server->lock, cursor->wlr->x, cursor->wlr->y, event->time_msec, pressed);
		wlr_seat_pointer_notify_button(server->input.seat, event->time_msec, event->button,
			event->state);
		return;
	}
	if (picker_active(&server->picker)) {
		picker_pointer_button(&server->picker, cursor->wlr->x, cursor->wlr->y, pressed);
		return;
	}
	xbridge_pointer_input(&server->xbridge, true);
	if (drag_button(&server->input.drag, event->button, pressed)) {
		return;
	}
	if (dnd_active(&server->input.dnd)) {
		// dropped on an X window flame takes care of it, else the drag grab
		// drops or cancels on the release
		if (!pressed && xselection_dnd_drop_x(&server->xselection, event->button,
				event->time_msec)) {
			return;
		}
		wlr_seat_pointer_notify_button(server->input.seat, event->time_msec,
			event->button, event->state);
		return;
	}
	if (xshell_has_pointer(&server->xshell)) {
		// focus first, then the click: the app hears FocusIn before the
		// press like under any X window manager. a button already held is
		// the implicit grab, the focus stays
		if (pressed && !xshell_buttons_held(&server->xshell)
				&& !xshell_pointer_grabbed(&server->xshell)) {
			double sx, sy;
			struct wlr_surface *surface;
			struct flame_view *view = desktop_view_at(&server->desktop,
				cursor->wlr->x, cursor->wlr->y, &surface, &sx, &sy);
			if (view) {
				focus_view(&server->desktop, view);
			}
		}
		xshell_pointer_button(&server->xshell, event->button, pressed, event->time_msec);
		return;
	}

	// on a wayland window: X apps listening for raw buttons hear it (push
	// to talk on a side button)
	xshell_button_elsewhere(&server->xshell, event->button, pressed, event->time_msec);
	if (pressed) {
		double sx, sy;
		struct wlr_surface *surface;
		struct flame_view *view = desktop_view_at(&server->desktop,
			cursor->wlr->x, cursor->wlr->y, &surface, &sx, &sy);
		if (surface && server->input.seat->pointer_state.focused_surface != surface) {
			// an open menu held the pointer back, the click lands where it is,
			// so clicking outside the menu still closes it
			wlr_seat_pointer_notify_enter(server->input.seat, surface, sx, sy);
		}
		if (surface) {
			xbridge_pointer_pressed(&server->xbridge, surface);
		}
		if (view) {
			// a view glued inside it (steam's store page) takes the keys
			view_pressed(view, surface);
			focus_view(&server->desktop, view);
		} else if (surface) {
			layers_offer_keyboard(&server->layers, surface);
		}
	}
	wlr_seat_pointer_notify_button(server->input.seat, event->time_msec,
		event->button, event->state);
	if (!pressed && server->input.seat->pointer_state.button_count == 0) {
		// the grab is over: whatever is under the pointer now gets it
		process_motion(cursor, event->time_msec, false);
	}
}

static void handle_button(struct wl_listener *listener, void *data) {
	struct flame_cursor *cursor = wl_container_of(listener, cursor, button);
	button_event(cursor, data);
}

static void axis_event(struct flame_cursor *cursor, struct wlr_pointer_axis_event *event) {
	if (drag_active(&cursor->server->input.drag) || picker_active(&cursor->server->picker)) {
		return;
	}
	if (!lock_active(&cursor->server->lock) && xshell_pointer_axis(&cursor->server->xshell,
			event->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL, event->delta,
			event->time_msec)) {
		return;
	}
	wlr_seat_pointer_notify_axis(cursor->server->input.seat, event->time_msec,
		event->orientation, event->delta, event->delta_discrete, event->source,
		event->relative_direction);
}

static void handle_axis(struct wl_listener *listener, void *data) {
	struct flame_cursor *cursor = wl_container_of(listener, cursor, axis);
	axis_event(cursor, data);
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
	// pointer left every client, nobody's going to set an image for us.
	// over X, X says what it looks like
	struct flame_cursor *cursor = wl_container_of(listener, cursor, pointer_focus_change);
	struct wlr_seat_pointer_focus_change_event *event = data;
	if (!event->new_surface && !xshell_has_pointer(&cursor->server->xshell)) {
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

void cursor_forget_view(struct flame_cursor *cursor, struct flame_view *view) {
	if (cursor->over == view) {
		cursor->over = NULL;
	}
}

void cursor_warp(struct flame_cursor *cursor, double x, double y) {
	wlr_cursor_warp_closest(cursor->wlr, NULL, x, y);
	process_motion(cursor, now_msec(), false);
	wlr_seat_pointer_notify_frame(cursor->server->input.seat);
}

// each one a whole event, the way a mouse frames them: clients hold pointer
// events until the frame that ends them
void cursor_test_move(struct flame_cursor *cursor, double x, double y) {
	wlr_cursor_warp_closest(cursor->wlr, NULL, x, y);
	confine(cursor);
	process_motion(cursor, now_msec(), true);
	wlr_seat_pointer_notify_frame(cursor->server->input.seat);
}

void cursor_test_nudge(struct flame_cursor *cursor, double dx, double dy) {
	moved_by(cursor, NULL, now_msec(), dx, dy, dx, dy);
	wlr_seat_pointer_notify_frame(cursor->server->input.seat);
}

void cursor_test_button(struct flame_cursor *cursor, uint32_t button, bool pressed) {
	struct wlr_pointer_button_event event = {
		.time_msec = now_msec(),
		.button = button,
		.state = pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED,
	};
	button_event(cursor, &event);
	wlr_seat_pointer_notify_frame(cursor->server->input.seat);
}

void cursor_test_scroll(struct flame_cursor *cursor, double delta) {
	// a wheel: 15 a notch, 120ths for the discrete steps
	struct wlr_pointer_axis_event event = {
		.time_msec = now_msec(),
		.source = WL_POINTER_AXIS_SOURCE_WHEEL,
		.orientation = WL_POINTER_AXIS_VERTICAL_SCROLL,
		.relative_direction = WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL,
		.delta = delta,
		.delta_discrete = (int32_t)(delta / 15 * 120),
	};
	axis_event(cursor, &event);
	wlr_seat_pointer_notify_frame(cursor->server->input.seat);
}

void cursor_set_pixels(struct flame_cursor *cursor, uint64_t key,
		const struct flame_cursor_pixels *frames, size_t len, float scale) {
	cursor_image_set_pixels(&cursor->image, key, frames, len, scale);
}

void cursor_hide(struct flame_cursor *cursor) {
	cursor_image_set_surface(&cursor->image, NULL, 0, 0);
}

void cursor_set_name(struct flame_cursor *cursor, const char *name) {
	cursor_image_set_name(&cursor->image, name);
}
