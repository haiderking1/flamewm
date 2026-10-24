#ifndef FLAME_INPUT_CONSTRAINTS_H
#define FLAME_INPUT_CONSTRAINTS_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct flame_server;
struct wlr_pointer_constraint_v1;

// wayland games' mouse: the mouse's own motion as it comes (relative-pointer:
// looking around keeps going at the screen's edge), and the pointer locked
// in place or kept inside the window (pointer-constraints). a constraint
// holds while its surface has the keyboard: the user going elsewhere (a
// click, alt-tab, a launcher) lets the pointer go

struct flame_constraints {
	struct flame_server *server;
	struct wlr_pointer_constraints_v1 *manager;
	struct wlr_relative_pointer_manager_v1 *relative;
	struct wlr_pointer_constraint_v1 *active; // the one holding the pointer, NULL for none

	struct wl_listener new_constraint;
	struct wl_listener focus_change; // the seat's keyboard focus
	struct wl_listener active_destroy;
	struct wl_listener active_region;
};

bool constraints_init(struct flame_constraints *constraints, struct flame_server *server);
void constraints_finish(struct flame_constraints *constraints);

// the mouse moved dx, dy (unaccel: before acceleration, what games want):
// told as is to the surface under the pointer that asked for it, and *dx,
// *dy made what the pointer itself moves: no further than its region's edge
// while confined. true: it's locked, it doesn't move at all
bool constraints_motion(struct flame_constraints *constraints, uint32_t time_msec,
	double *dx, double *dy, double unaccel_dx, double unaccel_dy);

#endif
