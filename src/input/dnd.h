#ifndef FLAME_INPUT_DND_H
#define FLAME_INPUT_DND_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct flame_server;
struct flame_surface_node;
struct flame_tree;
struct wlr_drag;

// drag and drop between wayland apps (wl_data_device): an app starts a drag
// on a button it holds, wlroots' drag grab takes the pointer and talks to
// whatever surface flame says is under it, the app's drag icon rides along
// with the cursor above everything. X windows join in through xselection/
struct flame_dnd {
	struct flame_server *server;
	struct wlr_drag *drag; // the one going on, NULL for none
	struct flame_tree *icon_tree; // follows the cursor
	struct flame_surface_node *icon;
	int icon_dx, icon_dy; // where the app wants its icon, from the hotspot

	struct wl_listener request_start_drag;
	struct wl_listener start_drag;
	struct wl_listener drag_destroy;
	struct wl_listener icon_destroy;
	struct wl_listener icon_commit;
};

void dnd_init(struct flame_dnd *dnd, struct flame_server *server);
void dnd_finish(struct flame_dnd *dnd);
bool dnd_active(struct flame_dnd *dnd);
// the cursor moved: the icon follows
void dnd_motion(struct flame_dnd *dnd);

#endif
