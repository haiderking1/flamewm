#include "input/drag.h"

#include <linux/input-event-codes.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>

#include "config/config.h"
#include "core/server.h"

void drag_init(struct flame_drag *drag, struct flame_server *server) {
	drag->server = server;
	drag->view = NULL;
	drag->swallowed = 0;
}

bool drag_active(struct flame_drag *drag) {
	return drag->view != NULL;
}

static uint32_t held_mods(struct flame_server *server) {
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->input.seat);
	if (!keyboard) {
		return 0;
	}
	return wlr_keyboard_get_modifiers(keyboard) & ~(WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2);
}

// the tile under the cursor, ignoring the one riding on it
static struct flame_view *drop_target(struct flame_drag *drag) {
	struct flame_server *server = drag->server;
	struct wlr_cursor *cursor = server->input.cursor.wlr;
	struct flame_view *view;
	wl_list_for_each(view, &server->desktop.active->views, link) {
		if (view != drag->view && view->shown
				&& wlr_box_contains_point(&view->geometry, cursor->x, cursor->y)) {
			return view;
		}
	}
	return NULL;
}

static bool try_begin(struct flame_drag *drag, uint32_t button) {
	struct flame_server *server = drag->server;
	struct flame_cursor *cursor = &server->input.cursor;

	if (drag->view || button != BTN_LEFT
			|| held_mods(server) != server->config->drag_modifier) {
		return false;
	}
	double sx, sy;
	struct wlr_surface *surface;
	struct flame_view *view = desktop_view_at(&server->desktop,
		cursor->wlr->x, cursor->wlr->y, &surface, &sx, &sy);
	if (!view || !view->shown || view->workspace != server->desktop.active) {
		return false;
	}

	drag->view = view;
	drag->button = button;
	drag->start_x = cursor->wlr->x;
	drag->start_y = cursor->wlr->y;
	drag->origin_x = view->geometry.x;
	drag->origin_y = view->geometry.y;

	view->grabbed = true;
	focus_view(&server->desktop, view);
	// ride above every tile. all layer trees sit at 0,0 so it stays put
	wlr_scene_node_reparent(&view->tree->node, server->render.layers[FLAME_LAYER_FLOATING]);

	// clients don't get the pointer while we're holding a window
	wlr_seat_pointer_clear_focus(server->input.seat);
	wlr_cursor_set_xcursor(cursor->wlr, cursor->xcursor, "grabbing");
	return true;
}

void drag_motion(struct flame_drag *drag) {
	struct wlr_cursor *cursor = drag->server->input.cursor.wlr;
	wlr_scene_node_set_position(&drag->view->tree->node,
		drag->origin_x + (int)(cursor->x - drag->start_x),
		drag->origin_y + (int)(cursor->y - drag->start_y));
}

// hands the window back to its workspace, wherever it currently sits
static struct flame_view *let_go(struct flame_drag *drag) {
	struct flame_view *view = drag->view;
	drag->view = NULL;
	view->grabbed = false;
	if (view->workspace) {
		wlr_scene_node_reparent(&view->tree->node, view->workspace->tree);
	}
	return view;
}

static void restore_pointer(struct flame_drag *drag) {
	struct flame_cursor *cursor = &drag->server->input.cursor;
	// focus was already cleared, so no focus change will reset the image
	wlr_cursor_set_xcursor(cursor->wlr, cursor->xcursor, "default");
	cursor_refresh(cursor);
}

static void drop(struct flame_drag *drag) {
	struct flame_view *target = drop_target(drag);
	struct flame_view *view = let_go(drag);

	if (target) {
		// stays at the drop point until the transaction places both
		desktop_swap_views(&drag->server->desktop, view, target);
	} else {
		wlr_scene_node_set_position(&view->tree->node, view->geometry.x, view->geometry.y);
	}
	restore_pointer(drag);
}

static uint32_t button_bit(uint32_t button) {
	return button >= BTN_MOUSE && button < BTN_MOUSE + 32 ? 1u << (button - BTN_MOUSE) : 0;
}

bool drag_button(struct flame_drag *drag, uint32_t button, bool pressed) {
	uint32_t bit = button_bit(button);
	if (pressed) {
		// mid drag every press is ours
		if (drag->view || try_begin(drag, button)) {
			drag->swallowed |= bit;
			return true;
		}
		return false;
	}

	if (!(drag->swallowed & bit)) {
		return false; // pressed before the drag, the client is owed this release
	}
	drag->swallowed &= ~bit;
	if (drag->view && button == drag->button) {
		drop(drag);
	}
	return true;
}

void drag_cancel(struct flame_drag *drag) {
	if (!drag->view) {
		return;
	}
	struct flame_view *view = let_go(drag);
	wlr_scene_node_set_position(&view->tree->node, view->geometry.x, view->geometry.y);
	restore_pointer(drag);
}

void drag_forget(struct flame_drag *drag, struct flame_view *view) {
	if (drag->view == view) {
		drag_cancel(drag);
	}
}
