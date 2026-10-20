#include "shell/popup.h"

#include <stdlib.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "core/server.h"
#include "layers/layers.h"
#include "shell/xdg_tree.h"
#include "util/listener.h"

// where the surface at the bottom of a popup chain sits on screen, popup
// positions are relative to it
static bool root_origin(struct wlr_xdg_popup *xdg_popup, int *lx, int *ly) {
	struct wlr_surface *parent = xdg_popup->parent;
	struct wlr_xdg_popup *parent_popup;
	while (parent && (parent_popup = wlr_xdg_popup_try_from_wlr_surface(parent))) {
		parent = parent_popup->parent;
	}
	if (!parent) {
		return false;
	}

	struct wlr_xdg_toplevel *toplevel = wlr_xdg_toplevel_try_from_wlr_surface(parent);
	if (toplevel && toplevel->base->data) {
		struct flame_tree *tree = toplevel->base->data;
		struct flame_view *view = view_from_node(&tree->node);
		if (view) {
			return scene_node_coords(&view->content->node, lx, ly);
		}
	}
	struct flame_layer_surface *layer = layer_surface_from_wlr(parent);
	if (layer) {
		return scene_node_coords(&layer->tree->node, lx, ly);
	}
	return false;
}

// keep menus on screen instead of hanging off the edge
static void popup_unconstrain(struct flame_popup *popup) {
	struct flame_output *output = outputs_primary(&popup->server->outputs);
	int lx, ly;
	if (!output || !root_origin(popup->xdg, &lx, &ly)) {
		wlr_xdg_surface_schedule_configure(popup->xdg->base);
		return;
	}

	// the box has to be relative to the root surface's own origin
	struct wlr_box box = output->box;
	box.x -= lx;
	box.y -= ly;
	wlr_xdg_popup_unconstrain_from_box(popup->xdg, &box);
}

static void handle_commit(struct wl_listener *listener, void *data) {
	struct flame_popup *popup = wl_container_of(listener, popup, commit);
	if (popup->xdg->base->initial_commit) {
		popup_unconstrain(popup);
	}
}

static void handle_reposition(struct wl_listener *listener, void *data) {
	struct flame_popup *popup = wl_container_of(listener, popup, reposition);
	popup_unconstrain(popup);
}

static void handle_destroy(struct wl_listener *listener, void *data) {
	struct flame_popup *popup = wl_container_of(listener, popup, destroy);
	listener_disconnect(&popup->commit);
	listener_disconnect(&popup->reposition);
	listener_disconnect(&popup->destroy);
	free(popup);
}

void popup_create(struct flame_server *server, struct wlr_xdg_popup *xdg_popup,
		struct flame_tree *parent_tree) {
	struct flame_popup *popup = calloc(1, sizeof(*popup));
	if (!popup) {
		wlr_log(WLR_ERROR, "out of memory");
		return;
	}
	popup->server = server;
	popup->xdg = xdg_popup;

	// nested inside the parent's tree, so it moves and hides with it
	if (!xdg_tree_create(parent_tree, xdg_popup->base)) {
		wlr_log(WLR_ERROR, "out of memory");
		free(popup);
		return;
	}

	listener_connect(&popup->commit, &xdg_popup->base->surface->events.commit,
		handle_commit);
	listener_connect(&popup->reposition, &xdg_popup->events.reposition,
		handle_reposition);
	listener_connect(&popup->destroy, &xdg_popup->events.destroy, handle_destroy);
}
