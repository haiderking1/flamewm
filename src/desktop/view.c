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

void view_content_size(struct flame_view *view, struct wlr_box box, int *width, int *height) {
	int bw = view_config(view)->border_width;
	*width = MAX(box.width - 2 * bw, 1);
	*height = MAX(box.height - 2 * bw, 1);
}

uint32_t view_configure(struct flame_view *view, struct wlr_box box) {
	int width, height;
	view_content_size(view, box, &width, &height);
	return view->impl->configure(view, width, height);
}

static void place_rect(struct wlr_scene_rect *rect, int x, int y, int width, int height) {
	wlr_scene_node_set_position(&rect->node, x, y);
	wlr_scene_rect_set_size(rect, MAX(width, 0), MAX(height, 0));
}

void view_place(struct flame_view *view, struct wlr_box box) {
	int bw = view_config(view)->border_width;
	int w = box.width;
	int h = box.height;

	view->geometry = box;
	wlr_scene_node_set_position(&view->tree->node, box.x, box.y);

	// four strips instead of one filled rect so transparent clients
	// don't get the border color bleeding through them
	place_rect(view->border[FLAME_BORDER_TOP], 0, 0, w, bw);
	place_rect(view->border[FLAME_BORDER_BOTTOM], 0, h - bw, w, bw);
	place_rect(view->border[FLAME_BORDER_LEFT], 0, bw, bw, h - 2 * bw);
	place_rect(view->border[FLAME_BORDER_RIGHT], w - bw, bw, bw, h - 2 * bw);

	wlr_scene_node_set_position(&view->content->node, bw, bw);

	int width, height;
	view_content_size(view, box, &width, &height);
	view->impl->set_display_size(view, width, height);

	view->shown = true;
	wlr_scene_node_set_enabled(&view->tree->node, true);
}

void view_hide(struct flame_view *view) {
	view->shown = false;
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
