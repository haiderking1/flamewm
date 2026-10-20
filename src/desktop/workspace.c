#include "desktop/workspace.h"

#include <wlr/util/log.h>

#include "desktop/view.h"
#include "layout/tree.h"
#include "scene/scene.h"

void workspace_init(struct flame_workspace *workspace, int index,
		struct flame_tree *tiled, struct flame_tree *floating, struct flame_tree *fullscreen) {
	static const float black[4] = {0, 0, 0, 1};

	workspace->index = index;
	workspace->tree = scene_tree_create(tiled);
	workspace->float_tree = scene_tree_create(floating);
	workspace->fullscreen_tree = scene_tree_create(fullscreen);
	workspace->backdrop = scene_rect_create(workspace->fullscreen_tree, 0, 0, black);
	scene_node_set_enabled(&workspace->backdrop->node, false);
	workspace->layout = NULL;
	wl_list_init(&workspace->views);
	wl_list_init(&workspace->floats);
	workspace->last_focused = NULL;
	workspace->fullscreen = NULL;
	workspace_set_visible(workspace, false);
}

void workspace_set_visible(struct flame_workspace *workspace, bool visible) {
	scene_node_set_enabled(&workspace->tree->node, visible);
	scene_node_set_enabled(&workspace->float_tree->node, visible);
	scene_node_set_enabled(&workspace->fullscreen_tree->node, visible);
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
	view_reparent(view);
	return true;
}

void workspace_add_float(struct flame_workspace *workspace, struct flame_view *view) {
	wl_list_insert(workspace->floats.prev, &view->link);
	view->node = NULL;
	view->workspace = workspace;
	view_reparent(view);
}

int workspace_view_count(struct flame_workspace *workspace) {
	return wl_list_length(&workspace->views) + wl_list_length(&workspace->floats);
}

void workspace_remove_view(struct flame_workspace *workspace, struct flame_view *view) {
	if (view->node) {
		tree_remove(&workspace->layout, view->node);
	}
	view->node = NULL;
	wl_list_remove(&view->link);
	wl_list_init(&view->link);
	view->workspace = NULL;
	if (workspace->last_focused == view) {
		workspace->last_focused = NULL;
	}
	if (workspace->fullscreen == view) {
		workspace->fullscreen = NULL;
	}
}

// swap two links in place, neighbours need special care
static void swap_links(struct wl_list *a, struct wl_list *b) {
	if (a->next == b) {
		wl_list_remove(a);
		wl_list_insert(b, a);
	} else if (b->next == a) {
		wl_list_remove(b);
		wl_list_insert(a, b);
	} else {
		struct wl_list *a_prev = a->prev;
		struct wl_list *b_prev = b->prev;
		wl_list_remove(a);
		wl_list_remove(b);
		wl_list_insert(b_prev, a);
		wl_list_insert(a_prev, b);
	}
}

void workspace_swap_views(struct flame_workspace *workspace, struct flame_view *a,
		struct flame_view *b) {
	tree_swap(a->node, b->node);
	struct tree_node *node = a->node;
	a->node = b->node;
	b->node = node;
	// the list follows leaf order, so it swaps too
	swap_links(&a->link, &b->link);
}

struct flame_view *workspace_first_view(struct flame_workspace *workspace) {
	struct flame_view *view;
	if (!wl_list_empty(&workspace->views)) {
		return wl_container_of(workspace->views.next, view, link);
	}
	if (!wl_list_empty(&workspace->floats)) {
		return wl_container_of(workspace->floats.prev, view, link); // the top one
	}
	return NULL;
}
