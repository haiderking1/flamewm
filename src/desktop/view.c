#include "desktop/view.h"

#include <wlr/types/wlr_scene.h>

#include "config/config.h"
#include "core/server.h"
#include "util/macros.h"

static const struct flame_config *view_config(struct flame_view *view) {
	return view->desktop->server->config;
}

void view_init(struct flame_view *view, const struct flame_view_impl *impl,
		struct flame_desktop *desktop) {
	view->impl = impl;
	view->desktop = desktop;
	wl_list_init(&view->link);
	transaction_view_init(view);

	// parked in the hidden staging tree, and hidden itself until a
	// transaction places it so it never shows up at 0,0
	view->tree = wlr_scene_tree_create(desktop->staging);
	view->tree->node.data = view;
	wlr_scene_node_set_enabled(&view->tree->node, false);
	for (int i = 0; i < FLAME_BORDER_COUNT; i++) {
		view->border[i] = wlr_scene_rect_create(view->tree, 0, 0,
			view_config(view)->border_color);
	}
	view->content = wlr_scene_tree_create(view->tree);
}

void view_finish(struct flame_view *view) {
	wlr_scene_node_destroy(&view->tree->node);
}

static int border_width(struct flame_view *view, bool fullscreen) {
	return fullscreen ? 0 : view_config(view)->border_width;
}

void view_content_size(struct flame_view *view, struct wlr_box box, bool fullscreen,
		int *width, int *height) {
	int bw = border_width(view, fullscreen);
	*width = MAX(box.width - 2 * bw, 1);
	*height = MAX(box.height - 2 * bw, 1);
}

uint32_t view_configure(struct flame_view *view, struct wlr_box box, bool fullscreen) {
	int width, height;
	view_content_size(view, box, fullscreen, &width, &height);
	return view->impl->configure(view, width, height, fullscreen);
}

void view_reparent(struct flame_view *view) {
	if (view->grabbed || !view->workspace) {
		return; // a drag or the staging tree owns it
	}
	struct wlr_scene_tree *parent = view->fullscreen_shown
		? view->workspace->fullscreen_tree : view->workspace->tree;
	if (view->tree->node.parent != parent) {
		wlr_scene_node_reparent(&view->tree->node, parent);
	}
}

void view_content_changed(struct flame_view *view) {
	int bw = border_width(view, view->fullscreen_shown);
	int x = bw, y = bw;
	if (view->fullscreen_shown) {
		// a client that won't fill the output sits centered on the black
		int width, height;
		view->impl->content_size(view, &width, &height);
		x = MAX((view->geometry.width - width) / 2, 0);
		y = MAX((view->geometry.height - height) / 2, 0);
	}
	wlr_scene_node_set_position(&view->content->node, x, y);
}

static void place_rect(struct wlr_scene_rect *rect, int x, int y, int width, int height) {
	wlr_scene_node_set_position(&rect->node, x, y);
	wlr_scene_rect_set_size(rect, MAX(width, 0), MAX(height, 0));
}

void view_place(struct flame_view *view, struct wlr_box box, bool fullscreen) {
	int bw = border_width(view, fullscreen);
	int w = box.width;
	int h = box.height;

	view->geometry = box;
	view->fullscreen_shown = fullscreen;
	view_reparent(view);
	if (!view->grabbed) {
		wlr_scene_node_set_position(&view->tree->node, box.x, box.y);
	}

	// four strips instead of one filled rect so transparent clients
	// don't get the border color bleeding through them
	place_rect(view->border[FLAME_BORDER_TOP], 0, 0, w, bw);
	place_rect(view->border[FLAME_BORDER_BOTTOM], 0, h - bw, w, bw);
	place_rect(view->border[FLAME_BORDER_LEFT], 0, bw, bw, h - 2 * bw);
	place_rect(view->border[FLAME_BORDER_RIGHT], w - bw, bw, bw, h - 2 * bw);

	int width, height;
	view_content_size(view, box, fullscreen, &width, &height);
	view->impl->set_display_size(view, width, height);
	view_content_changed(view);

	view->shown = true;
	wlr_scene_node_set_enabled(&view->tree->node, true);
}

void view_hide(struct flame_view *view) {
	view->shown = false;
	view->fullscreen_shown = false;
	wlr_scene_node_set_enabled(&view->tree->node, false);
}

void view_set_focused(struct flame_view *view, bool focused) {
	const struct flame_config *config = view_config(view);
	const float *color = focused ? config->border_focused_color : config->border_color;
	for (int i = 0; i < FLAME_BORDER_COUNT; i++) {
		wlr_scene_rect_set_color(view->border[i], color);
	}
	view->impl->set_activated(view, focused);
}

void view_set_resizing(struct flame_view *view, bool resizing) {
	if (view->resizing != resizing) {
		view->resizing = resizing;
		view->impl->set_resizing(view, resizing);
	}
}

void view_close(struct flame_view *view) {
	view->impl->close(view);
}

struct wlr_surface *view_surface(struct flame_view *view) {
	return view->impl->surface(view);
}

struct flame_view *view_from_node(struct wlr_scene_node *node) {
	// only view trees get node.data, wlroots never touches it
	while (node) {
		if (node->data) {
			return node->data;
		}
		node = node->parent ? &node->parent->node : NULL;
	}
	return NULL;
}
