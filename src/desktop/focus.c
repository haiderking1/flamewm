#include "desktop/focus.h"

#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_scene.h>

#include "core/server.h"

void focus_restore_keyboard(struct flame_desktop *desktop) {
	struct flame_server *server = desktop->server;
	// an open launcher keeps typing, a clicked bar gives it back
	if (layers_keyboard_locked(&server->layers)) {
		input_keyboard_enter(&server->input, server->layers.focused->wlr->surface);
		return;
	}
	layers_drop_keyboard(&server->layers);
	input_keyboard_enter(&server->input,
		desktop->focused ? view_surface(desktop->focused) : NULL);
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
		// keeps its popups above the neighbours
		wlr_scene_node_raise_to_top(&view->tree->node);
	}
	focus_restore_keyboard(desktop);
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

	struct wl_list *head = &current->workspace->views;
	struct wl_list *link = direction > 0 ? current->link.next : current->link.prev;
	if (link == head) {
		link = direction > 0 ? head->next : head->prev;
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
