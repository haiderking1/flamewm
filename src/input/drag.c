#include "input/drag.h"

#include <linux/input-event-codes.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>

#include "config/config.h"
#include "core/server.h"
#include "layout/tree.h"

// a tile never gets squeezed below this by a resize drag
#define MIN_TILE_SIZE 80

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

static struct wlr_cursor *pointer(struct flame_drag *drag) {
	return drag->server->input.cursor.wlr;
}

static void set_cursor_image(struct flame_drag *drag, const char *name) {
	struct flame_cursor *cursor = &drag->server->input.cursor;
	wlr_cursor_set_xcursor(cursor->wlr, cursor->xcursor, name);
}

static void restore_pointer(struct flame_drag *drag) {
	// focus was cleared when the drag started, no focus change will reset it
	set_cursor_image(drag, "default");
	cursor_refresh(&drag->server->input.cursor);
}

// ---- move ----

// the tile under the cursor, ignoring the one riding on it
static struct flame_view *drop_target(struct flame_drag *drag) {
	struct wlr_cursor *cursor = pointer(drag);
	struct flame_view *view;
	wl_list_for_each(view, &drag->server->desktop.active->views, link) {
		if (view != drag->view && view->shown && !view->fullscreen_shown
				&& wlr_box_contains_point(&view->geometry, cursor->x, cursor->y)) {
			return view;
		}
	}
	return NULL;
}

static bool begin_move(struct flame_drag *drag, struct flame_view *view) {
	drag->origin_x = view->geometry.x;
	drag->origin_y = view->geometry.y;
	view->grabbed = true;
	// ride above every tile. all layer trees sit at 0,0 so it stays put
	scene_node_reparent(&view->tree->node,
		drag->server->render.layers[FLAME_LAYER_FLOATING]);
	set_cursor_image(drag, "grabbing");
	return true;
}

static void move_motion(struct flame_drag *drag) {
	struct wlr_cursor *cursor = pointer(drag);
	scene_node_set_position(&drag->view->tree->node,
		drag->origin_x + (int)(cursor->x - drag->start_x),
		drag->origin_y + (int)(cursor->y - drag->start_y));
}

// hands the window back to its workspace, wherever it currently sits
static struct flame_view *let_go(struct flame_drag *drag) {
	struct flame_view *view = drag->view;
	drag->view = NULL;
	view->grabbed = false;
	view_reparent(view);
	return view;
}

static void end_move(struct flame_drag *drag, bool drop) {
	struct flame_view *target = drop ? drop_target(drag) : NULL;
	struct flame_view *view = let_go(drag);

	if (target) {
		// stays at the drop point until the transaction places both
		desktop_swap_views(&drag->server->desktop, view, target);
	} else {
		scene_node_set_position(&view->tree->node, view->geometry.x, view->geometry.y);
	}
	restore_pointer(drag);
}

// ---- resize ----

// the split under the edge we want to move. an edge on the screen border
// has nothing to move, so the opposite edge takes over like hyprland does
static struct flame_drag_axis grab_axis(struct flame_view *view, enum tree_split split,
		bool far, bool *moves_far) {
	struct tree_node *node = tree_edge_split(view->node, split, far);
	*moves_far = far;
	if (!node) {
		node = tree_edge_split(view->node, split, !far);
		*moves_far = !far;
	}
	return (struct flame_drag_axis){
		.split = node,
		.start_size = node ? tree_first_size(node) : 0,
	};
}

static const char *resize_cursor(struct flame_drag *drag, bool right, bool bottom) {
	bool horizontal = drag->horizontal.split != NULL;
	bool vertical = drag->vertical.split != NULL;
	if (horizontal && vertical) {
		return bottom ? (right ? "se-resize" : "sw-resize") : (right ? "ne-resize" : "nw-resize");
	}
	return horizontal ? "ew-resize" : "ns-resize";
}

static bool begin_resize(struct flame_drag *drag, struct flame_view *view) {
	struct wlr_cursor *cursor = pointer(drag);
	// the quarter you grab decides which edges follow you
	bool right = cursor->x >= view->geometry.x + view->geometry.width / 2.0;
	bool bottom = cursor->y >= view->geometry.y + view->geometry.height / 2.0;

	drag->horizontal = grab_axis(view, TREE_SPLIT_VERTICAL, right, &right);
	drag->vertical = grab_axis(view, TREE_SPLIT_HORIZONTAL, bottom, &bottom);
	if (!drag->horizontal.split && !drag->vertical.split) {
		return false; // alone on the workspace, nothing to resize
	}
	set_cursor_image(drag, resize_cursor(drag, right, bottom));
	drag->resize_pending = false;
	view_set_resizing(view, true);
	// whatever's in flight lands now, from here on nothing waits
	desktop_arrange_live(&drag->server->desktop);
	return true;
}

static void resize_motion(struct flame_drag *drag) {
	// a mouse reports way faster than the screen redraws, and every layout
	// change is a configure to every client on that split. once a frame
	if (!drag->resize_pending) {
		drag->resize_pending = true;
		outputs_schedule_frame(&drag->server->outputs);
	}
}

static void apply_resize(struct flame_drag *drag) {
	struct wlr_cursor *cursor = pointer(drag);
	int gap = drag->server->config->gap;
	drag->resize_pending = false;

	// from where it started, not step by step, so nothing drifts. a dividing
	// line follows the cursor whichever side of it the window sits on
	if (drag->horizontal.split) {
		tree_set_first_size(drag->horizontal.split,
			drag->horizontal.start_size + (int)(cursor->x - drag->start_x),
			gap, MIN_TILE_SIZE);
	}
	if (drag->vertical.split) {
		tree_set_first_size(drag->vertical.split,
			drag->vertical.start_size + (int)(cursor->y - drag->start_y),
			gap, MIN_TILE_SIZE);
	}
	desktop_arrange_live(&drag->server->desktop);
}

// let go of the resize. the splits keep whatever size they got
static void stop_resize(struct flame_drag *drag) {
	drag->view = NULL;
	drag->resize_pending = false;
	desktop_end_live(&drag->server->desktop);
	restore_pointer(drag);
}

static void end_resize(struct flame_drag *drag) {
	// the last bit of motion since the previous frame still counts
	if (drag->resize_pending) {
		apply_resize(drag);
	}
	stop_resize(drag);
	// clients are probably still drawing the last size, from here on the
	// usual transaction rules hold again
	desktop_arrange(&drag->server->desktop);
}

void drag_frame(struct flame_drag *drag) {
	if (drag->view && drag->mode == FLAME_DRAG_RESIZE && drag->resize_pending) {
		apply_resize(drag);
	}
}

// ---- shared ----

static bool try_begin(struct flame_drag *drag, uint32_t button) {
	struct flame_server *server = drag->server;
	struct wlr_cursor *cursor = pointer(drag);

	if (drag->view || (button != BTN_LEFT && button != BTN_RIGHT)
			|| held_mods(server) != server->config->drag_modifier) {
		return false;
	}
	double sx, sy;
	struct wlr_surface *surface;
	struct flame_view *view = desktop_view_at(&server->desktop,
		cursor->x, cursor->y, &surface, &sx, &sy);
	if (!view || !view->shown || view->workspace != server->desktop.active
			|| view->fullscreen_shown || view->fullscreen_wanted) {
		return false;
	}

	drag->view = view;
	drag->button = button;
	drag->mode = button == BTN_LEFT ? FLAME_DRAG_MOVE : FLAME_DRAG_RESIZE;
	drag->start_x = cursor->x;
	drag->start_y = cursor->y;

	bool started = drag->mode == FLAME_DRAG_MOVE
		? begin_move(drag, view) : begin_resize(drag, view);
	if (!started) {
		drag->view = NULL;
		return false;
	}
	focus_view(&server->desktop, view);
	// clients don't get the pointer while we're holding a window
	wlr_seat_pointer_clear_focus(server->input.seat);
	return true;
}

void drag_motion(struct flame_drag *drag) {
	if (drag->mode == FLAME_DRAG_MOVE) {
		move_motion(drag);
	} else {
		resize_motion(drag);
	}
}

static void finish(struct flame_drag *drag, bool drop) {
	if (drag->mode == FLAME_DRAG_MOVE) {
		end_move(drag, drop);
	} else if (drop) {
		end_resize(drag);
	} else {
		// cancels come from unmaps and workspace switches, they arrange
		// themselves and the window might be on its way out
		stop_resize(drag);
	}
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
		finish(drag, true);
	}
	return true;
}

void drag_cancel(struct flame_drag *drag) {
	if (drag->view) {
		finish(drag, false);
	}
}

void drag_forget(struct flame_drag *drag, struct flame_view *view) {
	if (drag->view == view) {
		drag_cancel(drag);
	}
}

void drag_tree_changed(struct flame_drag *drag) {
	// the splits we hold may be about to be freed, so no last layout either.
	// whoever changed the tree arranges right after
	if (drag->view && drag->mode == FLAME_DRAG_RESIZE) {
		stop_resize(drag);
	}
}
