#ifndef FLAME_SHELL_POPUP_H
#define FLAME_SHELL_POPUP_H

#include <wayland-server-core.h>

struct flame_server;
struct wlr_xdg_popup;

struct flame_popup {
	struct flame_server *server;
	struct wlr_xdg_popup *xdg;

	struct wl_listener commit;
	struct wl_listener reposition;
	struct wl_listener destroy;
};

struct wlr_scene_tree;

// frees itself when the client destroys the popup. parent_tree is where it
// draws: its parent popup or window's tree, or a layer surface's popup tree
void popup_create(struct flame_server *server, struct wlr_xdg_popup *xdg_popup,
	struct wlr_scene_tree *parent_tree);

#endif
