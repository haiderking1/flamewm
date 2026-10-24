#include "desktop/desktop.h"

#include <wlr/util/log.h>

#include "anim/window.h"
#include "anim/workspace.h"
#include "config/config.h"
#include "core/server.h"
#include "layout/tree.h"

bool desktop_init(struct flame_desktop *desktop, struct flame_server *server) {
	struct flame_tree *tiled = server->render.layers[FLAME_LAYER_TILED];
	struct flame_tree *floating = server->render.layers[FLAME_LAYER_FLOATING];
	struct flame_tree *fullscreen = server->render.layers[FLAME_LAYER_FULLSCREEN];

	desktop->server = server;
	wl_signal_init(&desktop->events.changed);
	desktop->staging = scene_tree_create(tiled);
	if (!desktop->staging) {
		wlr_log(WLR_ERROR, "failed to create staging tree");
		return false;
	}
	scene_node_set_enabled(&desktop->staging->node, false);

	for (int i = 0; i < FLAME_WORKSPACE_COUNT; i++) {
		workspace_init(&desktop->workspaces[i], i, tiled, floating, fullscreen);
	}
	desktop->active = &desktop->workspaces[0];
	workspace_set_visible(desktop->active, true);

	return transaction_init(&desktop->transaction, server);
}

void desktop_finish(struct flame_desktop *desktop) {
	if (desktop->changed_idle) {
		wl_event_source_remove(desktop->changed_idle);
		desktop->changed_idle = NULL;
	}
	transaction_finish(&desktop->transaction);
}

static void handle_changed_idle(void *data) {
	struct flame_desktop *desktop = data;
	desktop->changed_idle = NULL;
	wl_signal_emit_mutable(&desktop->events.changed, desktop);
}

void desktop_notify(struct flame_desktop *desktop) {
	// a close can unmap, refocus and rearrange in one go, listeners only
	// need to hear about where it ended up
	if (!desktop->changed_idle) {
		desktop->changed_idle = wl_event_loop_add_idle(desktop->server->loop,
			handle_changed_idle, desktop);
	}
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
	tree_layout(workspace->layout, output->usable, server->config->gap);

	// the owner takes the whole output, everyone else keeps tiling behind it
	struct flame_view *owner = workspace->fullscreen;
	struct flame_view *view;
	wl_list_for_each(view, &workspace->views, link) {
		bool fullscreen = view == owner && view->fullscreen_wanted;
		transaction_add(txn, view, fullscreen ? output->box : view->node->box, fullscreen);
	}
	// floats land in the same frame as the tiles around them
	wl_list_for_each(view, &workspace->floats, link) {
		view->float_box = floating_on_screen(desktop, view->float_box);
		bool fullscreen = view == owner && view->fullscreen_wanted;
		transaction_add(txn, view, fullscreen ? output->box : view->float_box, fullscreen);
	}
	transaction_commit(txn);
}

void desktop_arrange_live(struct flame_desktop *desktop) {
	struct flame_server *server = desktop->server;
	struct flame_transaction *txn = &desktop->transaction;

	// land whatever's in flight. applying can start one more to catch up on a
	// deferred arrange, that one lands on the second go
	while (transaction_active(txn)) {
		transaction_flush(txn);
	}

	struct flame_output *output = outputs_primary(&server->outputs);
	if (!output) {
		return;
	}
	struct flame_workspace *workspace = desktop->active;
	tree_layout(workspace->layout, output->usable, server->config->gap);

	struct flame_view *view;
	wl_list_for_each(view, &workspace->views, link) {
		if (!view->shown || view->fullscreen_shown
				|| wlr_box_equal(&view->node->box, &view->geometry)) {
			continue;
		}
		// every window on a moving split is in the resize, not just the
		// grabbed one. same configure as the new size
		view_set_resizing(view, true);
		// shown with whatever it last drew, cropped to the new box
		view_configure(view, view->node->box, false);
		view_place(view, view->node->box, false);
	}
}

void desktop_end_live(struct flame_desktop *desktop) {
	// live arranging only ever touches the visible workspace, and anything
	// that switches or moves windows away ends it first
	struct flame_view *view;
	wl_list_for_each(view, &desktop->active->views, link) {
		view_set_resizing(view, false);
	}
}

void desktop_layout_applied(struct flame_desktop *desktop) {
	fullscreen_refresh_backdrops(desktop);
	// things moved under the pointer
	cursor_refresh(&desktop->server->input.cursor);
}

bool desktop_prospective_geometry(struct flame_desktop *desktop, struct wlr_box *box) {
	struct flame_server *server = desktop->server;
	struct flame_output *output = outputs_primary(&server->outputs);
	if (!output) {
		return false;
	}
	struct flame_workspace *workspace = desktop->active;
	*box = tree_prospective(workspace->layout, workspace_insert_target(workspace),
		output->usable, server->config->gap);
	return true;
}

void desktop_map_view(struct flame_desktop *desktop, struct flame_view *view) {
	struct flame_workspace *workspace = desktop->active;
	drag_tree_changed(&desktop->server->input.drag);
	// the user's rules for it (init.lua): float or not, which workspace,
	// fullscreen
	struct flame_window_rule rule;
	config_window_rules(desktop->server->config, view_app_id(view), view_title(view), &rule);
	view->rule = rule;
	if (rule.floating_set) {
		view->float_wanted = rule.floating;
	}
	if (rule.fullscreen) {
		view->fullscreen_wanted = true;
	}
	if (rule.workspace) {
		workspace = &desktop->workspaces[rule.workspace - 1];
	}
	if (view->float_wanted) {
		view->floating = true;
		view->float_box = floating_initial_box(desktop, view);
		view->has_float_box = true;
		workspace_add_float(workspace, view);
	} else if (!workspace_add_view(workspace, view)) {
		return;
	}
	if (workspace != desktop->active) {
		// sent off to another workspace: it lays out when that one shows
		workspace->last_focused = view;
		fullscreen_claim_if_wanted(desktop, view);
		desktop_notify(desktop);
		return;
	}
	fullscreen_claim_if_wanted(desktop, view);
	// a window opening behind someone's fullscreen doesn't steal the seat
	if (!workspace->fullscreen || workspace->fullscreen == view) {
		focus_view(desktop, view);
	}
	// stays hidden until the transaction places it next to its resized siblings
	desktop_arrange(desktop);
	desktop_notify(desktop);
}

void desktop_unmap_view(struct flame_desktop *desktop, struct flame_view *view) {
	struct flame_workspace *workspace = view->workspace;
	if (!workspace) {
		return;
	}
	struct wlr_box closed = view->geometry;

	drag_forget(&desktop->server->input.drag, view);
	drag_tree_changed(&desktop->server->input.drag);
	cursor_forget_view(&desktop->server->input.cursor, view);
	// a remap starts from whatever the client asks for next, not a stale claim
	view->fullscreen_wanted = false;
	view->floating = false;
	view->has_float_box = false;
	// its last picture plays the close, from where it is now
	if (!view->unmap_quietly && !desktop->server->terminating) {
		window_anim_snapshot_if_missing(view);
		window_anim_closed(view);
	} else {
		window_anim_discard(view);
	}
	workspace_remove_view(workspace, view);
	transaction_remove(&desktop->transaction, view);
	view_hide(view);
	scene_node_reparent(&view->tree->node, desktop->staging);
	fullscreen_refresh_backdrops(desktop);

	if (desktop->focused == view) {
		// drop it without deactivating, the surface may be on its way out
		desktop->focused = NULL;
		focus_view(desktop, focus_after_close(desktop, closed));
	}
	desktop_arrange(desktop);
	desktop_notify(desktop);
}

void desktop_swap_views(struct flame_desktop *desktop, struct flame_view *a,
		struct flame_view *b) {
	if (a == b || a->workspace != desktop->active || b->workspace != desktop->active) {
		return;
	}
	drag_tree_changed(&desktop->server->input.drag);
	workspace_swap_views(desktop->active, a, b);
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

	// a held window or a half applied layout make no sense on a hidden workspace
	drag_cancel(&desktop->server->input.drag);
	transaction_flush(&desktop->transaction);

	focus_view(desktop, NULL);
	struct flame_workspace *from = desktop->active;
	workspace_set_visible(from, false);
	desktop->active = target;
	workspace_set_visible(target, true);
	// the old one slides out of sight before it hides
	workspace_anim_switch(desktop, from, target);

	desktop_arrange(desktop);
	focus_view(desktop, focus_fallback(target));
	// a different set of windows is under the still cursor now. waiting for a
	// transaction doesn't work, there's none when nothing needs a new size,
	// and the scroll would keep going to a window on the old workspace
	cursor_refresh(&desktop->server->input.cursor);
	desktop_notify(desktop);
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

	drag_forget(&desktop->server->input.drag, view);
	drag_tree_changed(&desktop->server->input.drag);
	transaction_flush(&desktop->transaction);

	bool was_focused = desktop->focused == view;
	if (was_focused) {
		focus_view(desktop, NULL);
	}

	workspace_remove_view(source, view);
	if (view->floating) {
		workspace_add_float(target, view);
		target->last_focused = view;
	} else if (!workspace_add_view(target, view)) {
		// couldn't get a leaf over there, put it back where it was
		workspace_add_view(source, view);
	} else {
		target->last_focused = view;
	}
	// a fullscreen window stays fullscreen over there, displacing that owner
	fullscreen_claim_if_wanted(desktop, view);
	fullscreen_refresh_backdrops(desktop);

	if (was_focused) {
		focus_view(desktop, focus_fallback(desktop->active));
	}
	// target gets laid out when it becomes visible
	desktop_arrange(desktop);
	// the last window leaving needs no transaction, the pointer moves on now
	cursor_refresh(&desktop->server->input.cursor);
	desktop_notify(desktop);
}

struct flame_view *desktop_view_at(struct flame_desktop *desktop, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
	struct flame_node *node =
		scene_node_at(&desktop->server->render.scene, lx, ly, surface, sx, sy);
	return node ? view_from_node(node) : NULL;
}
