#include "input/dnd.h"

#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

#include "core/server.h"
#include "scene/scene.h"
#include "util/listener.h"

bool dnd_active(struct flame_dnd *dnd) {
	return dnd->drag != NULL;
}

void dnd_motion(struct flame_dnd *dnd) {
	if (!dnd->icon_tree) {
		return;
	}
	struct wlr_cursor *cursor = dnd->server->input.cursor.wlr;
	scene_node_set_position(&dnd->icon_tree->node, (int)cursor->x + dnd->icon_dx,
		(int)cursor->y + dnd->icon_dy);
}

static void icon_gone(struct flame_dnd *dnd) {
	listener_disconnect(&dnd->icon_destroy);
	listener_disconnect(&dnd->icon_commit);
	if (dnd->icon_tree) {
		scene_node_destroy(&dnd->icon_tree->node);
		dnd->icon_tree = NULL;
		dnd->icon = NULL;
	}
}

static void handle_icon_destroy(struct wl_listener *listener, void *data) {
	struct flame_dnd *dnd = wl_container_of(listener, dnd, icon_destroy);
	icon_gone(dnd);
}

// the app moved its icon's origin, attach's dx and dy add up
static void handle_icon_commit(struct wl_listener *listener, void *data) {
	struct flame_dnd *dnd = wl_container_of(listener, dnd, icon_commit);
	struct wlr_surface *surface = data;
	dnd->icon_dx += surface->current.dx;
	dnd->icon_dy += surface->current.dy;
	dnd_motion(dnd);
}

static void handle_drag_destroy(struct wl_listener *listener, void *data) {
	struct flame_dnd *dnd = wl_container_of(listener, dnd, drag_destroy);
	listener_disconnect(&dnd->drag_destroy);
	icon_gone(dnd);
	dnd->drag = NULL;
	// over an X window when it ended: that window hears it's over
	xselection_dnd_drag_gone(&dnd->server->xselection);
	// whatever is under the pointer now gets it back, and sets its cursor
	cursor_set_name(&dnd->server->input.cursor, "default");
	cursor_refresh(&dnd->server->input.cursor);
}

static void handle_start_drag(struct wl_listener *listener, void *data) {
	struct flame_dnd *dnd = wl_container_of(listener, dnd, start_drag);
	struct wlr_drag *drag = data;
	if (xselection_drag_is_x(drag)) {
		return; // flame steers that one for an X app, the pointer stays the X app's
	}
	dnd->drag = drag;
	listener_connect(&dnd->drag_destroy, &drag->events.destroy, handle_drag_destroy);
	// the app dragging lost the pointer, it can't set the cursor anymore
	cursor_set_name(&dnd->server->input.cursor, "grabbing");
	if (!drag->icon) {
		return;
	}
	struct flame_render *render = &dnd->server->render;
	dnd->icon_tree = scene_tree_create(render->layers[FLAME_LAYER_OVERLAY]);
	if (!dnd->icon_tree) {
		return;
	}
	dnd->icon = scene_surface_create(dnd->icon_tree, drag->icon->surface);
	if (!dnd->icon) {
		scene_node_destroy(&dnd->icon_tree->node);
		dnd->icon_tree = NULL;
		return;
	}
	scene_node_raise_to_top(&dnd->icon_tree->node);
	dnd->icon_dx = 0;
	dnd->icon_dy = 0;
	listener_connect(&dnd->icon_destroy, &drag->icon->events.destroy, handle_icon_destroy);
	listener_connect(&dnd->icon_commit, &drag->icon->surface->events.commit, handle_icon_commit);
	dnd_motion(dnd);
}

// an app wants to drag: only off a button press it really got
static void handle_request_start_drag(struct wl_listener *listener, void *data) {
	struct flame_dnd *dnd = wl_container_of(listener, dnd, request_start_drag);
	struct wlr_seat_request_start_drag_event *event = data;
	struct wlr_seat *seat = dnd->server->input.seat;
	if (wlr_seat_validate_pointer_grab_serial(seat, event->origin, event->serial)) {
		wlr_seat_start_pointer_drag(seat, event->drag, event->serial);
		return;
	}
	wlr_log(WLR_DEBUG, "dnd: a drag without a button held, refused");
	if (event->drag->source) {
		wlr_data_source_destroy(event->drag->source);
	}
}

void dnd_init(struct flame_dnd *dnd, struct flame_server *server) {
	dnd->server = server;
	wl_list_init(&dnd->drag_destroy.link);
	wl_list_init(&dnd->icon_destroy.link);
	wl_list_init(&dnd->icon_commit.link);
	struct wlr_seat *seat = server->input.seat;
	listener_connect(&dnd->request_start_drag, &seat->events.request_start_drag,
		handle_request_start_drag);
	listener_connect(&dnd->start_drag, &seat->events.start_drag, handle_start_drag);
}

void dnd_finish(struct flame_dnd *dnd) {
	listener_disconnect(&dnd->request_start_drag);
	listener_disconnect(&dnd->start_drag);
	listener_disconnect(&dnd->drag_destroy);
	icon_gone(dnd);
}
