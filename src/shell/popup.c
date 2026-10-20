#include "shell/popup.h"

#include <stdlib.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "core/server.h"
#include "util/listener.h"

// keep menus on screen instead of hanging off the edge
static void popup_unconstrain(struct flame_popup *popup) {
	struct wlr_scene_tree *tree = popup->xdg->base->data;
	struct flame_view *view = view_from_node(&tree->node);
	struct flame_output *output = outputs_primary(&popup->server->outputs);
	if (!view || !output) {
		wlr_xdg_surface_schedule_configure(popup->xdg->base);
		return;
	}

	// the box has to be relative to the toplevel's own origin
	int lx, ly;
	wlr_scene_node_coords(&view->content->node, &lx, &ly);
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

void popup_create(struct flame_server *server, struct wlr_xdg_popup *xdg_popup) {
	struct wlr_xdg_surface *parent = xdg_popup->parent
		? wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent)
		: NULL;
	if (!parent || !parent->data) {
		// parent isn't something we draw (layer-shell later), nowhere to put it
		wlr_log(WLR_DEBUG, "popup without a known parent, ignoring");
		return;
	}

	struct flame_popup *popup = calloc(1, sizeof(*popup));
	if (!popup) {
		wlr_log(WLR_ERROR, "out of memory");
		return;
	}
	popup->server = server;
	popup->xdg = xdg_popup;

	// nested inside the parent's tree, so it moves and hides with it
	struct wlr_scene_tree *parent_tree = parent->data;
	xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

	listener_connect(&popup->commit, &xdg_popup->base->surface->events.commit,
		handle_commit);
	listener_connect(&popup->reposition, &xdg_popup->events.reposition,
		handle_reposition);
	listener_connect(&popup->destroy, &xdg_popup->events.destroy, handle_destroy);
}
