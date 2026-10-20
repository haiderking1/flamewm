#include "shell/xdg_tree.h"

#include <stdlib.h>
#include <wlr/types/wlr_xdg_shell.h>

#include "scene/scene.h"
#include "util/listener.h"

static void update_position(struct flame_xdg_tree *xdg_tree) {
	struct wlr_xdg_surface *xdg = xdg_tree->xdg;
	scene_node_set_position(&xdg_tree->surface->node, -xdg->geometry.x, -xdg->geometry.y);
	if (xdg->role == WLR_XDG_SURFACE_ROLE_POPUP && xdg->popup) {
		scene_node_set_position(&xdg_tree->tree->node, xdg->popup->current.geometry.x,
			xdg->popup->current.geometry.y);
	}
}

static void handle_commit(struct wl_listener *listener, void *data) {
	struct flame_xdg_tree *xdg_tree = wl_container_of(listener, xdg_tree, commit);
	update_position(xdg_tree);
}

static void handle_xdg_destroy(struct wl_listener *listener, void *data) {
	struct flame_xdg_tree *xdg_tree = wl_container_of(listener, xdg_tree, xdg_destroy);
	// the tree's destroy handler frees the rest
	scene_node_destroy(&xdg_tree->tree->node);
}

static void handle_tree_destroy(struct wl_listener *listener, void *data) {
	struct flame_xdg_tree *xdg_tree = wl_container_of(listener, xdg_tree, tree_destroy);
	listener_disconnect(&xdg_tree->tree_destroy);
	listener_disconnect(&xdg_tree->xdg_destroy);
	listener_disconnect(&xdg_tree->commit);
	if (xdg_tree->xdg->data == xdg_tree->tree) {
		xdg_tree->xdg->data = NULL;
	}
	free(xdg_tree);
}

struct flame_xdg_tree *xdg_tree_create(struct flame_tree *parent, struct wlr_xdg_surface *xdg) {
	struct flame_xdg_tree *xdg_tree = calloc(1, sizeof(*xdg_tree));
	if (!xdg_tree) {
		return NULL;
	}
	xdg_tree->xdg = xdg;
	xdg_tree->tree = scene_tree_create(parent);
	if (!xdg_tree->tree) {
		free(xdg_tree);
		return NULL;
	}
	xdg_tree->surface = scene_surface_create(xdg_tree->tree, xdg->surface);
	if (!xdg_tree->surface) {
		scene_node_destroy(&xdg_tree->tree->node);
		free(xdg_tree);
		return NULL;
	}

	listener_connect(&xdg_tree->tree_destroy, &xdg_tree->tree->node.events.destroy,
		handle_tree_destroy);
	listener_connect(&xdg_tree->xdg_destroy, &xdg->events.destroy, handle_xdg_destroy);
	listener_connect(&xdg_tree->commit, &xdg->surface->events.commit, handle_commit);
	xdg->data = xdg_tree->tree;
	update_position(xdg_tree);
	return xdg_tree;
}
