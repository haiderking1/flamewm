#include "desktop/focus.h"

#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>

#include "core/server.h"

static void keyboard_enter(struct wlr_seat *seat, struct wlr_surface *surface) {
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	if (keyboard) {
		wlr_seat_keyboard_notify_enter(seat, surface, keyboard->keycodes,
			keyboard->num_keycodes, &keyboard->modifiers);
	} else {
		wlr_seat_keyboard_notify_enter(seat, surface, NULL, 0, NULL);
	}
}

void focus_view(struct flame_desktop *desktop, struct flame_view *view) {
	struct wlr_seat *seat = desktop->server->input.seat;

	if (view && view == desktop->focused) {
		return;
	}
	if (desktop->focused) {
		view_set_focused(desktop->focused, false);
	}
	desktop->focused = view;

	if (!view) {
		wlr_seat_keyboard_notify_clear_focus(seat);
		return;
	}

	view->workspace->last_focused = view;
	view_set_focused(view, true);
	// keeps its popups above the neighbours
	wlr_scene_node_raise_to_top(&view->tree->node);
	keyboard_enter(seat, view_surface(view));
}

void focus_cycle(struct flame_desktop *desktop, int direction) {
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
