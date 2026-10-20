#include "desktop/workspace.h"

#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#include "desktop/view.h"
#include "layout/tree.h"

void workspace_init(struct flame_workspace *workspace, int index,
		struct wlr_scene_tree *parent) {
	workspace->index = index;
	workspace->tree = wlr_scene_tree_create(parent);
	wlr_scene_node_set_enabled(&workspace->tree->node, false);
	workspace->layout = NULL;
	wl_list_init(&workspace->views);
	workspace->last_focused = NULL;
}

void workspace_set_visible(struct flame_workspace *workspace, bool visible) {
	wlr_scene_node_set_enabled(&workspace->tree->node, visible);
}

struct tree_node *workspace_insert_target(struct flame_workspace *workspace) {
	if (workspace->last_focused) {
		return workspace->last_focused->node;
	}
	return tree_last_leaf(workspace->layout);
}

bool workspace_add_view(struct flame_workspace *workspace, struct flame_view *view) {
	struct tree_node *target = workspace_insert_target(workspace);
	struct tree_node *leaf = tree_insert(&workspace->layout, target, view);
	if (!leaf) {
		wlr_log(WLR_ERROR, "out of memory");
		return false;
	}

	// the new leaf comes right after the one it split, keep the list matching
	if (target) {
		wl_list_insert(&target->view->link, &view->link);
	} else {
		wl_list_insert(workspace->views.prev, &view->link);
	}
	view->node = leaf;
	view->workspace = workspace;
	wlr_scene_node_reparent(&view->tree->node, workspace->tree);
	return true;
}

void workspace_remove_view(struct flame_workspace *workspace, struct flame_view *view) {
	tree_remove(&workspace->layout, view->node);
	view->node = NULL;
	wl_list_remove(&view->link);
	wl_list_init(&view->link);
	view->workspace = NULL;
	if (workspace->last_focused == view) {
		workspace->last_focused = NULL;
	}
}

struct flame_view *workspace_first_view(struct flame_workspace *workspace) {
	if (wl_list_empty(&workspace->views)) {
		return NULL;
	}
	struct flame_view *view = wl_container_of(workspace->views.next, view, link);
	return view;
}
