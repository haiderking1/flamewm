#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

#include "core/server.h"
#include "util/listener.h"
#include "xselection/internal.h"
#include "xserver/xserver.h"

// drag and drop between X and wayland apps, XDND (version 5) on the X side.
//
// an X app dragging onto a wayland app: X apps find where they're dragging
// over by X's own window tree, so while the pointer is over wayland flame
// shows a proxy window on top of every X window there. the X app talks XDND
// to it, flame plays that to the wayland app under the pointer through a
// wlroots drag it drives by hand. the pointer itself stays with the X app,
// it's holding the button
//
// a wayland app dragging onto an X app: over an X window that speaks XDND
// flame is the XDND source, the wayland drag's data answers XdndSelection.
// dropping there ends wlroots' drag without cancelling the wayland source,
// the X app's XdndFinished finishes it

#define XDND_VERSION 5
#define FINISH_TIMEOUT_MS 5000

struct xdnd_source; // an X drag offered to wayland

struct xsel_dnd {
	struct flame_xselection *owner;
	struct flame_xserver *xserver;
	struct {
		uint32_t aware, enter, position, status, leave, drop, finished, selection,
			type_list, copy, move, ask, atom;
	} atoms;

	// an X app dragging (X to wayland)
	bool x_dragging; // an X app owns XdndSelection
	struct xs_window *proxy;
	uint32_t x_source; // its window, from XdndEnter. 0 before
	struct wl_array x_mimes, x_targets; // what it offers
	uint32_t x_action; // what it asked for
	struct xdnd_source *source; // standing in for it on the wayland side
	struct wlr_drag *drag;
	struct wl_listener drag_destroy;

	// a wayland app dragging over an X app (wayland to X)
	uint32_t x_target; // the X top-level, 0 for none
	int target_version;
	bool status_pending; // a position went out, its status hasn't come
	bool position_pending; // the pointer moved meanwhile
	bool accepted;
	uint32_t target_action;
	struct wlr_data_source *wl_source; // the wayland app's, while dropped on X
	struct wl_listener wl_source_destroy;
	bool dropped; // waiting for XdndFinished
	struct wl_event_source *finish_timeout;
	uint32_t last_x, last_y;
};

// one X drag as wayland sees it: which X window it is and what it offers
// are its own, a second X drag can start while this one's drop is read
struct xdnd_source {
	struct wlr_data_source base;
	struct xsel_dnd *dnd;
	uint32_t x_source; // the X app's window
	struct wl_array targets; // uint32_t, one per mime type
	bool dropped, finished;
	struct wl_event_source *finish_idle;
};

static struct xsel_dnd *dnd_of(struct flame_xselection *xselection) {
	return xselection->dnd;
}

static struct wlr_seat *seat_of(struct xsel_dnd *dnd) {
	return dnd->owner->server->input.seat;
}

static uint32_t action_atom(struct xsel_dnd *dnd, uint32_t wl_action) {
	switch (wl_action) {
	case WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY:
		return dnd->atoms.copy;
	case WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE:
		return dnd->atoms.move;
	case WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK:
		return dnd->atoms.ask;
	default:
		return 0;
	}
}

static uint32_t wl_action_of(struct xsel_dnd *dnd, uint32_t atom) {
	if (atom == dnd->atoms.move) {
		return WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
	}
	if (atom == dnd->atoms.ask) {
		return WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK;
	}
	return WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
}

static void send(struct xsel_dnd *dnd, uint32_t to, uint32_t type, uint32_t a, uint32_t b,
		uint32_t c, uint32_t d, uint32_t e) {
	const uint32_t data[5] = {a, b, c, d, e};
	xserver_send_client_message(dnd->xserver, to, type, data);
}

// ---- an X app dragging over wayland ----

static void x_drag_reset(struct xsel_dnd *dnd);

static void source_send(struct wlr_data_source *base, const char *mime, int32_t fd) {
	struct xdnd_source *source = wl_container_of(base, source, base);
	struct xsel_dnd *dnd = source->dnd;
	if (!dnd) {
		close(fd);
		return;
	}
	xsel_fetch(dnd->owner, dnd->atoms.selection,
		xsel_target_of(&base->mime_types, &source->targets, mime), fd);
}

static void source_accept(struct wlr_data_source *base, uint32_t serial, const char *mime) {
	// the next status tells the X app
}

static void source_free_idle(void *data) {
	struct xdnd_source *source = data;
	source->finish_idle = NULL;
	wlr_data_source_destroy(&source->base);
}

static void source_dnd_drop(struct wlr_data_source *base) {
	struct xdnd_source *source = wl_container_of(base, source, base);
	source->dropped = true;
}

// the wayland app has it: the X app hears it went through
static void source_dnd_finish(struct wlr_data_source *base) {
	struct xdnd_source *source = wl_container_of(base, source, base);
	struct xsel_dnd *dnd = source->dnd;
	if (dnd && source->x_source && !source->finished) {
		source->finished = true;
		send(dnd, source->x_source, dnd->atoms.finished, dnd->proxy->id, 1,
			action_atom(dnd, base->current_dnd_action), 0, 0);
	}
	if (!source->finish_idle && dnd) {
		source->finish_idle = wl_event_loop_add_idle(dnd->owner->server->loop,
			source_free_idle, source);
	}
}

static void source_destroy(struct wlr_data_source *base) {
	struct xdnd_source *source = wl_container_of(base, source, base);
	struct xsel_dnd *dnd = source->dnd;
	// dropped and never finished (the wayland app went, or dropped its
	// offer): the X app is still waiting to hear, it didn't go through
	if (dnd && source->dropped && !source->finished && source->x_source) {
		send(dnd, source->x_source, dnd->atoms.finished, dnd->proxy->id, 0, 0, 0, 0);
	}
	if (source->finish_idle) {
		wl_event_source_remove(source->finish_idle);
	}
	if (dnd && dnd->source == source) {
		dnd->source = NULL;
	}
	wl_array_release(&source->targets);
	free(source);
}

static const struct wlr_data_source_impl source_impl = {
	.send = source_send,
	.accept = source_accept,
	.destroy = source_destroy,
	.dnd_drop = source_dnd_drop,
	.dnd_finish = source_dnd_finish,
};

bool xselection_drag_is_x(struct wlr_drag *drag) {
	return drag && drag->source && drag->source->impl == &source_impl;
}

static void handle_drag_destroy(struct wl_listener *listener, void *data) {
	struct xsel_dnd *dnd = wl_container_of(listener, dnd, drag_destroy);
	listener_disconnect(&dnd->drag_destroy);
	dnd->drag = NULL;
}

// the wayland side of the X app's drag, made on the first wayland surface
// it gets over: a drag needs a client to belong to
static bool start_wayland_drag(struct xsel_dnd *dnd, struct wlr_surface *surface) {
	struct wlr_seat *seat = seat_of(dnd);
	if (seat->drag) {
		return false; // a wayland drag is going on, it has the seat
	}
	struct wlr_seat_client *client = wlr_seat_client_for_wl_client(seat,
		wl_resource_get_client(surface->resource));
	if (!client) {
		return false;
	}
	struct xdnd_source *source = calloc(1, sizeof(*source));
	if (!source) {
		return false;
	}
	source->dnd = dnd;
	source->x_source = dnd->x_source;
	wlr_data_source_init(&source->base, &source_impl);
	wl_array_init(&source->targets);
	char **each;
	size_t i = 0;
	wl_array_for_each(each, &dnd->x_mimes) {
		char **slot = wl_array_add(&source->base.mime_types, sizeof(*slot));
		uint32_t *target = wl_array_add(&source->targets, sizeof(*target));
		if (!slot || !target || !(*slot = strdup(*each))) {
			wlr_data_source_destroy(&source->base);
			return false;
		}
		*target = ((uint32_t *)dnd->x_targets.data)[i++];
	}
	// what the X app asked for: a copy stays a copy, a move can be a copy
	// too, ask leaves it to the target
	uint32_t action = wl_action_of(dnd, dnd->x_action);
	source->base.actions = action == WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY
		? action : action | WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY;
	if (action == WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK) {
		source->base.actions |= WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE;
	}
	struct wlr_drag *drag = wlr_drag_create(client, &source->base, NULL);
	if (!drag) {
		wlr_data_source_destroy(&source->base);
		return false;
	}
	// its pointer grab never starts, the seat it would have had it goes in
	// by hand: the drop reads the seat's button state through it
	drag->pointer_grab.seat = seat;
	dnd->source = source;
	dnd->drag = drag;
	listener_connect(&dnd->drag_destroy, &drag->events.destroy, handle_drag_destroy);
	// no pointer grab: the pointer is the X app's, flame steers this one
	wlr_seat_start_drag(seat, drag, wl_display_next_serial(dnd->owner->server->display));
	return true;
}

static void x_status(struct xsel_dnd *dnd) {
	bool accepted = dnd->drag && dnd->drag->focus && dnd->source && dnd->source->base.accepted
		&& dnd->source->base.current_dnd_action;
	uint32_t action = accepted ? action_atom(dnd, dnd->source->base.current_dnd_action) : 0;
	// an empty rectangle: keep the positions coming
	send(dnd, dnd->x_source, dnd->atoms.status, dnd->proxy->id, (accepted ? 1 : 0) | 2, 0, 0,
		action);
}

static void x_enter(struct xsel_dnd *dnd, const uint32_t data[5]) {
	struct flame_xselection *xselection = dnd->owner;
	dnd->x_source = data[0];
	char **old;
	wl_array_for_each(old, &dnd->x_mimes) {
		free(*old);
	}
	wl_array_release(&dnd->x_mimes);
	wl_array_release(&dnd->x_targets);
	wl_array_init(&dnd->x_mimes);
	wl_array_init(&dnd->x_targets);
	uint32_t types[64];
	size_t count = 0;
	struct xs_window *source = xserver_window(dnd->xserver, data[0]);
	struct xs_property *list = source && (data[1] & 1)
		? xserver_property(dnd->xserver, source, "XdndTypeList") : NULL;
	if (list && list->format == 32) {
		count = list->units < 64 ? list->units : 64;
		memcpy(types, list->data, count * 4);
	} else {
		for (int i = 2; i < 5; i++) {
			if (data[i]) {
				types[count++] = data[i];
			}
		}
	}
	for (size_t i = 0; i < count; i++) {
		const char *mime = xsel_mime_of(xselection, types[i]);
		if (mime) {
			xsel_add_offer(&dnd->x_mimes, &dnd->x_targets, mime, types[i]);
		}
	}
}

static void x_position(struct xsel_dnd *dnd, const uint32_t data[5]) {
	struct flame_server *server = dnd->owner->server;
	double x = data[2] >> 16, y = data[2] & 0xffff;
	dnd->x_action = data[4];
	double sx, sy;
	struct wlr_surface *surface = NULL;
	desktop_view_at(&server->desktop, x, y, &surface, &sx, &sy);
	if (surface && !dnd->drag && dnd->x_mimes.size) {
		start_wayland_drag(dnd, surface);
	}
	if (dnd->drag) {
		struct wlr_seat_pointer_grab *grab = &dnd->drag->pointer_grab;
		if (surface) {
			grab->interface->enter(grab, surface, sx, sy);
			grab->interface->motion(grab, data[3], sx, sy);
		} else {
			grab->interface->clear_focus(grab);
		}
	}
	x_status(dnd);
}

static void x_leave(struct xsel_dnd *dnd) {
	if (dnd->source && !dnd->source->dropped) {
		wlr_data_source_destroy(&dnd->source->base); // the drag goes with it
	}
	dnd->x_source = 0;
}

static void x_drop(struct xsel_dnd *dnd, const uint32_t data[5]) {
	struct wlr_drag *drag = dnd->drag;
	bool accepted = drag && drag->focus && dnd->source && dnd->source->base.accepted
		&& dnd->source->base.current_dnd_action;
	if (!accepted) {
		if (dnd->source) {
			dnd->source->finished = true; // told here, not again on destroy
		}
		send(dnd, dnd->x_source, dnd->atoms.finished, dnd->proxy->id, 0, 0, 0, 0);
		x_leave(dnd);
		return;
	}
	// the release wlroots' drag grab drops on, as if it had the pointer
	struct wlr_seat *seat = seat_of(dnd);
	seat->pointer_state.grab_button = BTN_LEFT;
	drag->pointer_grab.interface->button(&drag->pointer_grab, data[2], BTN_LEFT,
		WL_POINTER_BUTTON_STATE_RELEASED);
	// XdndFinished goes out when the wayland app finishes
}

// an X app started or ended a drag: XdndSelection is its
bool xsel_dnd_owner_changed(struct flame_xselection *xselection, uint32_t selection,
		bool x_owned) {
	struct xsel_dnd *dnd = dnd_of(xselection);
	if (!dnd || selection != dnd->atoms.selection) {
		return false;
	}
	if (x_owned) {
		dnd->x_dragging = true;
		xselection_pointer_moved(xselection);
	} else {
		x_drag_reset(dnd);
	}
	return true;
}

static void x_drag_reset(struct xsel_dnd *dnd) {
	dnd->x_dragging = false;
	xserver_flame_window_hide(dnd->xserver, dnd->proxy);
	if (dnd->source && !dnd->source->dropped) {
		wlr_data_source_destroy(&dnd->source->base);
	}
	dnd->x_source = 0;
}

// the proxy covers the screen while an X app drags over wayland, and gets
// out of the way over X windows
void xselection_pointer_moved(struct flame_xselection *xselection) {
	struct xsel_dnd *dnd = dnd_of(xselection);
	if (!dnd || !dnd->x_dragging) {
		return;
	}
	struct flame_server *server = xselection->server;
	struct wlr_cursor *cursor = server->input.cursor.wlr;
	bool over_x = xshell_window_at(&server->xshell, cursor->x, cursor->y) != 0;
	if (over_x || !server->xshell.buttons) {
		if (dnd->proxy->mapped) {
			xserver_flame_window_hide(dnd->xserver, dnd->proxy);
		}
		return;
	}
	if (!dnd->proxy->mapped) {
		struct flame_xserver *x = dnd->xserver;
		xserver_flame_window_show(x, dnd->proxy, 0, 0, x->root->width, x->root->height);
	}
}

// ---- a wayland app dragging over X ----

static void wl_source_forget(struct xsel_dnd *dnd) {
	listener_disconnect(&dnd->wl_source_destroy);
	dnd->wl_source = NULL;
}

static void wl_drag_end(struct xsel_dnd *dnd);

static void handle_wl_source_destroy(struct wl_listener *listener, void *data) {
	struct xsel_dnd *dnd = wl_container_of(listener, dnd, wl_source_destroy);
	wl_source_forget(dnd);
	// the drop the X app was reading has nothing behind it anymore
	if (dnd->dropped) {
		wl_drag_end(dnd);
	}
}

static void wl_drag_end(struct xsel_dnd *dnd) {
	if (dnd->finish_timeout) {
		wl_event_source_remove(dnd->finish_timeout);
		dnd->finish_timeout = NULL;
	}
	xserver_selection_own(dnd->xserver, dnd->atoms.selection, false);
	struct xs_window *flames = xserver_window(dnd->xserver, XS_FLAME_WINDOW);
	if (flames) {
		xserver_delete_property(dnd->xserver, flames, dnd->atoms.type_list);
	}
	dnd->x_target = 0;
	dnd->dropped = false;
	dnd->accepted = false;
	dnd->status_pending = false;
	dnd->position_pending = false;
	wl_source_forget(dnd);
}

static struct wlr_data_source *wayland_drag_source(struct xsel_dnd *dnd) {
	if (dnd->wl_source) {
		return dnd->wl_source;
	}
	struct wlr_drag *drag = seat_of(dnd)->drag;
	return drag && !xselection_drag_is_x(drag) ? drag->source : NULL;
}

static void x_target_leave(struct xsel_dnd *dnd) {
	if (dnd->x_target && !dnd->dropped) {
		send(dnd, dnd->x_target, dnd->atoms.leave, XS_FLAME_WINDOW, 0, 0, 0, 0);
		struct wlr_data_source *source = wayland_drag_source(dnd);
		if (source && dnd->accepted) {
			wlr_data_source_accept(source, 0, NULL);
		}
		wl_drag_end(dnd);
	}
}

void xselection_dnd_drag_gone(struct flame_xselection *xselection) {
	struct xsel_dnd *dnd = dnd_of(xselection);
	if (dnd && !dnd->dropped) {
		x_target_leave(dnd);
	}
}

void xselection_dnd_left_x(struct flame_xselection *xselection) {
	struct xsel_dnd *dnd = dnd_of(xselection);
	if (dnd) {
		x_target_leave(dnd);
	}
}

static uint32_t wayland_action(struct xsel_dnd *dnd, struct wlr_data_source *source) {
	if (source->actions & WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY || !source->actions) {
		return dnd->atoms.copy;
	}
	return action_atom(dnd, source->actions & WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE
		? WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE : WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK);
}

static void send_position(struct xsel_dnd *dnd, uint32_t time) {
	struct wlr_data_source *source = wayland_drag_source(dnd);
	if (!source) {
		return;
	}
	if (dnd->status_pending) {
		dnd->position_pending = true; // one at a time, the latest goes next
		return;
	}
	dnd->status_pending = true;
	dnd->position_pending = false;
	send(dnd, dnd->x_target, dnd->atoms.position, XS_FLAME_WINDOW, 0,
		dnd->last_x << 16 | dnd->last_y, time, wayland_action(dnd, source));
}

// which XDND version the X window speaks, 0 for none
static int aware_version(struct xsel_dnd *dnd, uint32_t window) {
	struct xs_window *x = xserver_window(dnd->xserver, window);
	struct xs_property *aware = x ? xserver_property(dnd->xserver, x, "XdndAware") : NULL;
	if (!aware || aware->format != 32 || aware->units < 1) {
		return 0;
	}
	uint32_t version;
	memcpy(&version, aware->data, 4);
	return version >= 3 ? (int)version : 0;
}

static void x_target_enter(struct xsel_dnd *dnd, uint32_t window, int version,
		struct wlr_data_source *source) {
	dnd->x_target = window;
	dnd->target_version = version < XDND_VERSION ? version : XDND_VERSION;
	dnd->accepted = false;
	dnd->target_action = 0;
	xserver_selection_own(dnd->xserver, dnd->atoms.selection, true);
	// the types, as atoms: all of them in XdndTypeList, the first three in
	// the message
	uint32_t types[64];
	uint32_t count = 0;
	char **each;
	wl_array_for_each(each, &source->mime_types) {
		if (count < 64 && strchr(*each, '/')) {
			types[count++] = xserver_atom(dnd->xserver, *each);
		}
		if (count < 62 && !strcmp(*each, "text/plain;charset=utf-8")) {
			types[count++] = xserver_atom(dnd->xserver, "UTF8_STRING");
			types[count++] = xserver_atom(dnd->xserver, "STRING");
		}
	}
	struct xs_window *flames = xserver_window(dnd->xserver, XS_FLAME_WINDOW);
	if (flames) {
		xserver_set_property(dnd->xserver, flames, dnd->atoms.type_list, dnd->atoms.atom, 32,
			types, count);
	}
	send(dnd, window, dnd->atoms.enter, XS_FLAME_WINDOW,
		(uint32_t)dnd->target_version << 24 | (count > 3 ? 1 : 0), count > 0 ? types[0] : 0,
		count > 1 ? types[1] : 0, count > 2 ? types[2] : 0);
}

bool xselection_dnd_over_x(struct flame_xselection *xselection, double lx, double ly,
		uint32_t time) {
	struct xsel_dnd *dnd = dnd_of(xselection);
	struct wlr_data_source *source = dnd ? wayland_drag_source(dnd) : NULL;
	if (!source || dnd->dropped) {
		return false;
	}
	uint32_t window = xshell_window_at(&xselection->server->xshell, lx, ly);
	int version = window ? aware_version(dnd, window) : 0;
	if (!version) {
		x_target_leave(dnd);
		return false;
	}
	if (window != dnd->x_target) {
		x_target_leave(dnd);
		x_target_enter(dnd, window, version, source);
	}
	dnd->last_x = (uint32_t)lx & 0xffff;
	dnd->last_y = (uint32_t)ly & 0xffff;
	send_position(dnd, time);
	return true;
}

// the drop didn't go through: the wayland app hears cancelled, so a move
// doesn't delete anything
static void wl_drop_failed(struct xsel_dnd *dnd) {
	struct wlr_data_source *source = dnd->wl_source;
	wl_drag_end(dnd);
	if (source) {
		wlr_data_source_destroy(source);
	}
}

static int handle_finish_timeout(void *data) {
	struct xsel_dnd *dnd = data;
	dnd->finish_timeout = NULL;
	wlr_log(WLR_DEBUG, "xdnd: the X app never finished the drop");
	wl_drop_failed(dnd);
	return 0;
}

bool xselection_dnd_drop_x(struct flame_xselection *xselection, uint32_t button,
		uint32_t time) {
	struct xsel_dnd *dnd = dnd_of(xselection);
	struct wlr_seat *seat = dnd ? seat_of(dnd) : NULL;
	struct wlr_drag *drag = seat ? seat->drag : NULL;
	// only the button the drag is held with drops, and only once
	if (!dnd || !dnd->x_target || dnd->dropped || !drag || xselection_drag_is_x(drag)
			|| button != seat->pointer_state.grab_button) {
		return false;
	}
	if (!dnd->accepted || !drag->source) {
		x_target_leave(dnd);
		return false; // wlroots cancels it
	}
	// keep the wayland source through the drag's end: it's the X app's to
	// read now
	dnd->wl_source = drag->source;
	listener_connect(&dnd->wl_source_destroy, &drag->source->events.destroy,
		handle_wl_source_destroy);
	dnd->dropped = true;
	// ending the grab ends the drag and leaves the source be, the release
	// still counts for the seat
	wlr_seat_pointer_end_grab(seat);
	wlr_seat_pointer_notify_button(seat, time, button, WL_POINTER_BUTTON_STATE_RELEASED);
	wlr_data_source_dnd_action(dnd->wl_source, wl_action_of(dnd, dnd->target_action));
	wlr_data_source_dnd_drop(dnd->wl_source);
	send(dnd, dnd->x_target, dnd->atoms.drop, XS_FLAME_WINDOW, 0, time, 0, 0);
	dnd->finish_timeout = wl_event_loop_add_timer(xselection->server->loop,
		handle_finish_timeout, dnd);
	if (dnd->finish_timeout) {
		wl_event_source_timer_update(dnd->finish_timeout, FINISH_TIMEOUT_MS);
	}
	return true;
}

static void send_wayland(void *source, const char *mime, int fd) {
	wlr_data_source_send(source, mime, fd);
}

bool xselection_dnd_request(struct flame_xselection *xselection,
		struct xs_selection_request *request) {
	struct xsel_dnd *dnd = dnd_of(xselection);
	if (!dnd || request->selection != dnd->atoms.selection) {
		return false;
	}
	struct wlr_data_source *source = wayland_drag_source(dnd);
	if (!source) {
		xserver_selection_answer(xselection->xserver, request, 0, 8, NULL, 0);
		return true;
	}
	xsel_answer(xselection, request, &source->mime_types, send_wayland, source);
	return true;
}

// ---- messages to flame's windows ----

void xselection_client_message(struct flame_xselection *xselection, uint32_t window,
		uint32_t type, const uint32_t data[5]) {
	struct xsel_dnd *dnd = dnd_of(xselection);
	if (!dnd) {
		return;
	}
	if (window == dnd->proxy->id) {
		if (type == dnd->atoms.enter) {
			x_enter(dnd, data);
		} else if (type == dnd->atoms.position && data[0] == dnd->x_source) {
			x_position(dnd, data);
		} else if (type == dnd->atoms.leave && data[0] == dnd->x_source) {
			x_leave(dnd);
		} else if (type == dnd->atoms.drop && data[0] == dnd->x_source) {
			x_drop(dnd, data);
		}
		return;
	}
	if (window != XS_FLAME_WINDOW || data[0] != dnd->x_target || !dnd->x_target) {
		return;
	}
	struct wlr_data_source *source = wayland_drag_source(dnd);
	if (type == dnd->atoms.status) {
		dnd->status_pending = false;
		dnd->accepted = data[1] & 1;
		dnd->target_action = data[4];
		if (source && !dnd->dropped) {
			const char *mime = NULL;
			if (dnd->accepted && source->mime_types.size) {
				mime = ((char **)source->mime_types.data)[0];
			}
			wlr_data_source_accept(source, 0, mime);
			wlr_data_source_dnd_action(source, dnd->accepted
				? wl_action_of(dnd, dnd->target_action) : 0);
		}
		if (dnd->position_pending && !dnd->dropped) {
			send_position(dnd, 0);
		}
	} else if (type == dnd->atoms.finished && dnd->dropped) {
		if (!(data[1] & 1)) {
			wl_drop_failed(dnd); // the X app didn't take it
			return;
		}
		struct wlr_data_source *finished = dnd->wl_source;
		if (finished && data[2]) {
			wlr_data_source_dnd_action(finished, wl_action_of(dnd, data[2]));
		}
		wl_drag_end(dnd);
		if (finished) {
			wlr_data_source_dnd_finish(finished);
		}
	}
}

// ---- setup ----

bool xsel_dnd_init(struct flame_xselection *xselection) {
	struct xsel_dnd *dnd = calloc(1, sizeof(*dnd));
	if (!dnd) {
		return false;
	}
	struct flame_xserver *x = xselection->xserver;
	dnd->owner = xselection;
	dnd->xserver = x;
	dnd->atoms.aware = xserver_atom(x, "XdndAware");
	dnd->atoms.enter = xserver_atom(x, "XdndEnter");
	dnd->atoms.position = xserver_atom(x, "XdndPosition");
	dnd->atoms.status = xserver_atom(x, "XdndStatus");
	dnd->atoms.leave = xserver_atom(x, "XdndLeave");
	dnd->atoms.drop = xserver_atom(x, "XdndDrop");
	dnd->atoms.finished = xserver_atom(x, "XdndFinished");
	dnd->atoms.selection = xserver_atom(x, "XdndSelection");
	dnd->atoms.type_list = xserver_atom(x, "XdndTypeList");
	dnd->atoms.copy = xserver_atom(x, "XdndActionCopy");
	dnd->atoms.move = xserver_atom(x, "XdndActionMove");
	dnd->atoms.ask = xserver_atom(x, "XdndActionAsk");
	dnd->atoms.atom = xserver_atom(x, "ATOM");
	wl_array_init(&dnd->x_mimes);
	wl_array_init(&dnd->x_targets);
	wl_list_init(&dnd->drag_destroy.link);
	wl_list_init(&dnd->wl_source_destroy.link);
	dnd->proxy = xserver_flame_window_create(x);
	uint32_t version = XDND_VERSION;
	if (!dnd->proxy || !xserver_set_property(x, dnd->proxy, dnd->atoms.aware, dnd->atoms.atom,
			32, &version, 1)) {
		if (dnd->proxy) {
			xserver_flame_window_destroy(x, dnd->proxy);
		}
		free(dnd);
		return false;
	}
	xselection->dnd = dnd;
	return true;
}

void xsel_dnd_finish(struct flame_xselection *xselection) {
	struct xsel_dnd *dnd = dnd_of(xselection);
	if (!dnd) {
		return;
	}
	if (dnd->finish_timeout) {
		wl_event_source_remove(dnd->finish_timeout);
	}
	listener_disconnect(&dnd->drag_destroy);
	listener_disconnect(&dnd->wl_source_destroy);
	if (dnd->source) {
		dnd->source->dnd = NULL;
	}
	char **each;
	wl_array_for_each(each, &dnd->x_mimes) {
		free(*each);
	}
	wl_array_release(&dnd->x_mimes);
	wl_array_release(&dnd->x_targets);
	xserver_flame_window_destroy(dnd->xserver, dnd->proxy);
	free(dnd);
	xselection->dnd = NULL;
}
