#include "desktop/desktop.h"

#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#include "config/config.h"
#include "core/server.h"
#include "layout/tree.h"

bool desktop_init(struct flame_desktop *desktop, struct flame_server *server) {
	struct wlr_scene_tree *tiled = server->render.layers[FLAME_LAYER_TILED];

	desktop->server = server;
	desktop->staging = wlr_scene_tree_create(tiled);
	if (!desktop->staging) {
		wlr_log(WLR_ERROR, "failed to create staging tree");
		return false;
	}
	wlr_scene_node_set_enabled(&desktop->staging->node, false);

	for (int i = 0; i < FLAME_WORKSPACE_COUNT; i++) {
		workspace_init(&desktop->workspaces[i], i, tiled);
	}
	desktop->active = &desktop->workspaces[0];
	workspace_set_visible(desktop->active, true);

	return transaction_init(&desktop->transaction, server);
}

void desktop_finish(struct flame_desktop *desktop) {
	transaction_finish(&desktop->transaction);
}

void desktop_arrange(struct flame_desktop *desktop) {
	struct flame_server *server = desktop->server;
	struct flame_transaction *txn = &desktop->transaction;

	// one change at a time, the next one starts from what's on screen
	if (transaction_active(txn)) {
		txn->rearrange = true;
		return;
	}

	struct flame_output *output = outputs_primary(&server->outputs);
	if (!output) {
		return;
	}

	struct flame_workspace *workspace = desktop->active;
	tree_layout(workspace->layout, output->box, server->config->gap);

	struct flame_view *view;
	wl_list_for_each(view, &workspace->views, link) {
		transaction_add(txn, view, view->node->box);
	}
	transaction_commit(txn);
}

bool desktop_prospective_geometry(struct flame_desktop *desktop, struct wlr_box *box) {
	struct flame_server *server = desktop->server;
	struct flame_output *output = outputs_primary(&server->outputs);
	if (!output) {
		return false;
	}
	struct flame_workspace *workspace = desktop->active;
	*box = tree_prospective(workspace->layout, workspace_insert_target(workspace),
		output->box, server->config->gap);
	return true;
}

void desktop_map_view(struct flame_desktop *desktop, struct flame_view *view) {
	if (!workspace_add_view(desktop->active, view)) {
		return;
	}
	focus_view(desktop, view);
	// stays hidden until the transaction places it next to its resized siblings
	desktop_arrange(desktop);
}

void desktop_unmap_view(struct flame_desktop *desktop, struct flame_view *view) {
	struct flame_workspace *workspace = view->workspace;
	if (!workspace) {
		return;
	}

	workspace_remove_view(workspace, view);
	transaction_remove(&desktop->transaction, view);
	view_hide(view);
	wlr_scene_node_reparent(&view->tree->node, desktop->staging);

	if (desktop->focused == view) {
		// drop it without deactivating, the surface may be on its way out
		desktop->focused = NULL;
		focus_view(desktop, focus_fallback(desktop->active));
	}
	desktop_arrange(desktop);
}

void desktop_switch_workspace(struct flame_desktop *desktop, int index) {
	if (index < 0 || index >= FLAME_WORKSPACE_COUNT) {
		return;
	}
	struct flame_workspace *target = &desktop->workspaces[index];
	if (target == desktop->active) {
		return;
	}

	// a half applied layout on a workspace we're leaving is just noise
	transaction_flush(&desktop->transaction);

	focus_view(desktop, NULL);
	workspace_set_visible(desktop->active, false);
	desktop->active = target;
	workspace_set_visible(target, true);

	desktop_arrange(desktop);
	focus_view(desktop, focus_fallback(target));
}

void desktop_move_view_to_workspace(struct flame_desktop *desktop,
		struct flame_view *view, int index) {
	if (index < 0 || index >= FLAME_WORKSPACE_COUNT) {
		return;
	}
	struct flame_workspace *source = view->workspace;
	struct flame_workspace *target = &desktop->workspaces[index];
	if (!source || source == target) {
		return;
	}

	transaction_flush(&desktop->transaction);

	bool was_focused = desktop->focused == view;
	if (was_focused) {
		focus_view(desktop, NULL);
	}

	workspace_remove_view(source, view);
	if (!workspace_add_view(target, view)) {
		// couldn't get a leaf over there, put it back where it was
		workspace_add_view(source, view);
	} else {
		target->last_focused = view;
	}

	if (was_focused) {
		focus_view(desktop, focus_fallback(desktop->active));
	}
	// target gets laid out when it becomes visible
	desktop_arrange(desktop);
}

struct flame_view *desktop_view_at(struct flame_desktop *desktop, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
	struct wlr_scene *scene = desktop->server->render.scene;
	struct wlr_scene_node *node = wlr_scene_node_at(&scene->tree.node, lx, ly, sx, sy);

	*surface = NULL;
	if (!node) {
		return NULL;
	}
	if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_surface *scene_surface =
			wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
		if (scene_surface) {
			*surface = scene_surface->surface;
		}
	}
	return view_from_node(node);
}
