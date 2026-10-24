#include "input/constraints.h"

#include <math.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>

#include "core/server.h"
#include "scene/surface.h"
#include "util/listener.h"

// where surface's origin is on screen (layout coords), as the last frame
// had it. false: it isn't on screen
static bool origin(struct wlr_surface *surface, double *x, double *y) {
	struct flame_surface_state *state = scene_surface_state(surface);
	if (!state || !state->drawn_seq) {
		return false;
	}
	*x = state->drawn_x;
	*y = state->drawn_y;
	return true;
}

static void forget_active(struct flame_constraints *constraints) {
	constraints->active = NULL;
	listener_disconnect(&constraints->active_destroy);
	listener_disconnect(&constraints->active_region);
}

// the active one lets the pointer go. a locked pointer comes out where the
// game says its own cursor was (its cursor hint), not where it was locked
static void deactivate(struct flame_constraints *constraints) {
	struct wlr_pointer_constraint_v1 *active = constraints->active;
	if (!active) {
		return;
	}
	forget_active(constraints);
	wlr_log(WLR_DEBUG, "constraints: the pointer's let go");
	double ox, oy;
	if (active->type == WLR_POINTER_CONSTRAINT_V1_LOCKED
			&& active->current.cursor_hint.enabled && origin(active->surface, &ox, &oy)) {
		cursor_warp(&constraints->server->input.cursor, ox + active->current.cursor_hint.x,
			oy + active->current.cursor_hint.y);
	}
	wlr_pointer_constraint_v1_send_deactivated(active); // a oneshot one is gone after this
}

// a confined pointer outside its region (it was confined somewhere else):
// into the middle of the region
static void confine_now(struct flame_constraints *constraints) {
	struct wlr_pointer_constraint_v1 *active = constraints->active;
	double ox, oy;
	if (!active || active->type != WLR_POINTER_CONSTRAINT_V1_CONFINED
			|| !pixman_region32_not_empty(&active->region)
			|| !origin(active->surface, &ox, &oy)) {
		return;
	}
	struct wlr_cursor *cursor = constraints->server->input.cursor.wlr;
	if (pixman_region32_contains_point(&active->region, (int)(cursor->x - ox),
			(int)(cursor->y - oy), NULL)) {
		return;
	}
	pixman_box32_t *box = pixman_region32_extents(&active->region);
	cursor_warp(&constraints->server->input.cursor, ox + (box->x1 + box->x2) / 2.0,
		oy + (box->y1 + box->y2) / 2.0);
}

static void handle_active_destroy(struct wl_listener *listener, void *data) {
	struct flame_constraints *constraints =
		wl_container_of(listener, constraints, active_destroy);
	forget_active(constraints);
}

static void handle_active_region(struct wl_listener *listener, void *data) {
	struct flame_constraints *constraints =
		wl_container_of(listener, constraints, active_region);
	confine_now(constraints);
}

static void activate(struct flame_constraints *constraints,
		struct wlr_pointer_constraint_v1 *constraint) {
	if (constraints->active == constraint) {
		return;
	}
	deactivate(constraints);
	if (!constraint) {
		return;
	}
	wlr_log(WLR_DEBUG, "constraints: the pointer's %s",
		constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED ? "locked" : "confined");
	constraints->active = constraint;
	listener_connect(&constraints->active_destroy, &constraint->events.destroy,
		handle_active_destroy);
	listener_connect(&constraints->active_region, &constraint->events.set_region,
		handle_active_region);
	wlr_pointer_constraint_v1_send_activated(constraint);
	confine_now(constraints);
}

// the constraint of the surface with the keyboard holds, any other lets go
static void refocus(struct flame_constraints *constraints) {
	struct wlr_seat *seat = constraints->server->input.seat;
	struct wlr_surface *surface = seat->keyboard_state.focused_surface;
	activate(constraints, surface ? wlr_pointer_constraints_v1_constraint_for_surface(
		constraints->manager, surface, seat) : NULL);
}

static void handle_focus_change(struct wl_listener *listener, void *data) {
	struct flame_constraints *constraints = wl_container_of(listener, constraints, focus_change);
	refocus(constraints);
}

static void handle_new_constraint(struct wl_listener *listener, void *data) {
	struct flame_constraints *constraints =
		wl_container_of(listener, constraints, new_constraint);
	refocus(constraints); // asked for by the surface with the keyboard: it holds right away
}

bool constraints_init(struct flame_constraints *constraints, struct flame_server *server) {
	constraints->server = server;
	// connected only while a constraint's active, let go of either way
	wl_list_init(&constraints->active_destroy.link);
	wl_list_init(&constraints->active_region.link);
	constraints->manager = wlr_pointer_constraints_v1_create(server->display);
	constraints->relative = wlr_relative_pointer_manager_v1_create(server->display);
	if (!constraints->manager || !constraints->relative) {
		wlr_log(WLR_ERROR, "failed to create the pointer constraints");
		return false;
	}
	listener_connect(&constraints->new_constraint, &constraints->manager->events.new_constraint,
		handle_new_constraint);
	listener_connect(&constraints->focus_change,
		&server->input.seat->keyboard_state.events.focus_change, handle_focus_change);
	return true;
}

void constraints_finish(struct flame_constraints *constraints) {
	forget_active(constraints);
	listener_disconnect(&constraints->new_constraint);
	listener_disconnect(&constraints->focus_change);
}

bool constraints_motion(struct flame_constraints *constraints, uint32_t time_msec, double *dx,
		double *dy, double unaccel_dx, double unaccel_dy) {
	wlr_relative_pointer_manager_v1_send_relative_motion(constraints->relative,
		constraints->server->input.seat, (uint64_t)time_msec * 1000, *dx, *dy, unaccel_dx,
		unaccel_dy);
	struct wlr_pointer_constraint_v1 *active = constraints->active;
	if (!active) {
		return false;
	}
	if (active->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
		*dx = 0;
		*dy = 0;
		return true;
	}
	double ox, oy;
	if (!origin(active->surface, &ox, &oy) || !pixman_region32_not_empty(&active->region)) {
		return false;
	}
	struct wlr_cursor *cursor = constraints->server->input.cursor.wlr;
	double sx = cursor->x - ox, sy = cursor->y - oy;
	// outside its region (it wasn't known yet when the confining began): on
	// its nearest edge first, the motion from there
	if (!pixman_region32_contains_point(&active->region, (int)floor(sx), (int)floor(sy),
			NULL)) {
		pixman_box32_t *box = pixman_region32_extents(&active->region);
		sx = fmin(fmax(sx, box->x1), box->x2 - 1);
		sy = fmin(fmax(sy, box->y1), box->y2 - 1);
	}
	double tx, ty;
	if (!wlr_region_confine(&active->region, sx, sy, sx + *dx, sy + *dy, &tx, &ty)) {
		tx = sx;
		ty = sy;
	}
	*dx = ox + tx - cursor->x;
	*dy = oy + ty - cursor->y;
	return false;
}
