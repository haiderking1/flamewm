#ifndef FLAME_SHELL_XDG_TREE_H
#define FLAME_SHELL_XDG_TREE_H

#include <wayland-server-core.h>

struct flame_surface_node;
struct flame_tree;
struct wlr_xdg_surface;

// an xdg surface in the scene: a tree whose origin is the window geometry's
// corner (csd shadows hang off the top left), and for popups, sitting where
// the popup goes relative to its parent. keeps up on every commit and goes
// away with the xdg surface, or with whatever tree it's in
struct flame_xdg_tree {
	struct flame_tree *tree;
	struct flame_surface_node *surface;
	struct wlr_xdg_surface *xdg;

	struct wl_listener tree_destroy;
	struct wl_listener xdg_destroy;
	struct wl_listener commit;
};

// xdg->data is set to the tree, popups find their parent's through it
struct flame_xdg_tree *xdg_tree_create(struct flame_tree *parent, struct wlr_xdg_surface *xdg);

#endif
