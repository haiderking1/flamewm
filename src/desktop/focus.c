#include "desktop/focus.h"

#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_layer_shell_v1.h>

#include "config/config.h"
#include "core/server.h"
#include "layout/tree.h"

void focus_restore_keyboard(struct flame_desktop *desktop) {
	struct flame_server *server = desktop->server;
	// an open launcher keeps typing, a clicked bar gives it back
	if (layers_keyboard_locked(&server->layers)) {
		input_keyboard_enter(&server->input, server->layers.focused->wlr->surface);
		return;
	}
	layers_drop_keyboard(&server->layers);
	input_keyboard_enter(&server->input,
		desktop->focused ? view_keyboard_surface(desktop->focused) : NULL);
}

void focus_view(struct flame_desktop *desktop, struct flame_view *view) {
	if (view && view == desktop->focused) {
		focus_restore_keyboard(desktop);
		return;
	}
	if (desktop->focused) {
		view_set_focused(desktop->focused, false);
	}
	desktop->focused = view;

	if (view) {
		view->workspace->last_focused = view;
		view_set_focused(view, true);
		// keeps its popups above the neighbours, a float above the others.
		// the floats list keeps its order: that's what focus cycles through
		scene_node_raise_to_top(&view->tree->node);
	}
	focus_restore_keyboard(desktop);
	desktop_notify(desktop);
}

void focus_cycle(struct flame_desktop *desktop, int direction) {
	// everything else is behind it, cycling there would type into the void
	if (desktop->active->fullscreen) {
		return;
	}
	struct flame_view *current = desktop->focused;
	if (!current) {
		focus_view(desktop, focus_fallback(desktop->active));
		return;
	}

	// tiles in leaf order, then the floats, round and round
	struct flame_workspace *workspace = current->workspace;
	struct wl_list *lists[2] = {&workspace->views, &workspace->floats};
	int in = current->floating ? 1 : 0;
	struct wl_list *link = direction > 0 ? current->link.next : current->link.prev;
	for (int hops = 0; link == lists[in] && hops < 2; hops++) {
		in = !in; // off the end of one list onto the other
		link = direction > 0 ? lists[in]->next : lists[in]->prev;
	}
	if (link == lists[in]) {
		return; // it's alone
	}
	struct flame_view *next = wl_container_of(link, next, link);
	focus_view(desktop, next);
}

struct flame_view *focus_fallback(struct flame_workspace *workspace) {
	if (workspace->last_focused) {
		return workspace->last_focused;
	}
	return workspace_first_view(workspace);
}

// how far p is from box, 0 inside it
static long distance_sq(struct wlr_box box, double x, double y) {
	double dx = x < box.x ? box.x - x : x > box.x + box.width ? x - box.x - box.width : 0;
	double dy = y < box.y ? box.y - y : y > box.y + box.height ? y - box.y - box.height : 0;
	return (long)(dx * dx + dy * dy);
}

static struct flame_view *nearest(struct flame_workspace *workspace, double x, double y) {
	struct flame_view *best = NULL;
	long best_distance = 0;
	struct flame_view *view;
	wl_list_for_each(view, &workspace->views, link) {
		long distance = distance_sq(view->node->box, x, y);
		if (!best || distance < best_distance) {
			best = view;
			best_distance = distance;
		}
	}
	return best;
}

struct flame_view *focus_after_close(struct flame_desktop *desktop, struct wlr_box closed) {
	struct flame_server *server = desktop->server;
	struct flame_workspace *workspace = desktop->active;
	struct flame_output *output = outputs_primary(&server->outputs);
	if (workspace->fullscreen) {
		return workspace->fullscreen; // the only one you can see
	}
	// the float on top under the pointer, floats sit above everything
	struct wlr_cursor *pointer = server->input.cursor.wlr;
	struct flame_view *view;
	struct flame_node *node;
	wl_list_for_each_reverse(node, &workspace->float_tree->children, link) {
		view = view_from_node(node);
		if (view && server->config->focus_follows_mouse
				&& wlr_box_contains_point(&view->float_box, pointer->x, pointer->y)) {
			return view;
		}
	}
	if (!output || wl_list_empty(&workspace->views)) {
		return focus_fallback(workspace);
	}
	// where everyone lands once the gap closes. the arrange right after this
	// does the same math, nothing's placed until its transaction lands
	tree_layout(workspace->layout, output->usable, server->config->gap);

	// with follow mouse, whoever ends up under the cursor. it never moved,
	// so the next nudge would focus it anyway, and jumping somewhere else in
	// between is what feels wrong (hyprland's focus_on_close = 1)
	struct wlr_cursor *cursor = server->input.cursor.wlr;
	if (server->config->focus_follows_mouse) {
		wl_list_for_each(view, &workspace->views, link) {
			if (wlr_box_contains_point(&view->node->box, cursor->x, cursor->y)) {
				return view;
			}
		}
	}
	// else the one closest to where it was, usually whoever took its space
	// (hyprland's default, focus_on_close = 0)
	return nearest(workspace, closed.x + closed.width / 2.0, closed.y + closed.height / 2.0);
}
