#include "desktop/fullscreen.h"

#include <wlr/types/wlr_scene.h>

#include "core/server.h"

bool fullscreen_area(struct flame_desktop *desktop, struct wlr_box *box) {
	struct flame_output *output = outputs_primary(&desktop->server->outputs);
	if (!output || wlr_box_empty(&output->box)) {
		return false;
	}
	*box = output->box;
	return true;
}

// the previous owner drops back into its tile in the same transaction
static void claim(struct flame_workspace *workspace, struct flame_view *view) {
	struct flame_view *previous = workspace->fullscreen;
	if (previous && previous != view) {
		previous->fullscreen_wanted = false;
	}
	workspace->fullscreen = view;
	view->fullscreen_wanted = true;
}

bool fullscreen_request(struct flame_desktop *desktop, struct flame_view *view,
		bool fullscreen) {
	struct flame_workspace *workspace = view->workspace;
	if (!workspace) {
		view->fullscreen_wanted = fullscreen;
		return true;
	}
	if (fullscreen && workspace->fullscreen != view && desktop->focused != view) {
		return false;
	}

	// a held window can't also be fullscreen
	drag_forget(&desktop->server->input.drag, view);

	if (fullscreen) {
		claim(workspace, view);
	} else {
		view->fullscreen_wanted = false;
		if (workspace->fullscreen == view) {
			workspace->fullscreen = NULL;
		}
	}
	// hidden workspaces sort themselves out when they show up
	if (workspace == desktop->active) {
		desktop_arrange(desktop);
	}
	desktop_notify(desktop);
	return true;
}

void fullscreen_toggle(struct flame_desktop *desktop) {
	struct flame_view *view = desktop->focused;
	if (view) {
		fullscreen_request(desktop, view, !view->fullscreen_wanted);
	}
}

void fullscreen_claim_if_wanted(struct flame_desktop *desktop, struct flame_view *view) {
	if (view->workspace && view->fullscreen_wanted) {
		claim(view->workspace, view);
	}
}

void fullscreen_refresh_backdrops(struct flame_desktop *desktop) {
	struct wlr_box area;
	bool have_area = fullscreen_area(desktop, &area);

	for (int i = 0; i < FLAME_WORKSPACE_COUNT; i++) {
		struct flame_workspace *workspace = &desktop->workspaces[i];
		// a displaced owner stays fullscreen on screen until its transaction lands
		bool any = false;
		struct flame_view *view;
		wl_list_for_each(view, &workspace->views, link) {
			any |= view->fullscreen_shown;
		}

		bool on = any && have_area;
		wlr_scene_node_set_enabled(&workspace->backdrop->node, on);
		if (on) {
			wlr_scene_node_set_position(&workspace->backdrop->node, area.x, area.y);
			wlr_scene_rect_set_size(workspace->backdrop, area.width, area.height);
			wlr_scene_node_lower_to_bottom(&workspace->backdrop->node);
		}
	}
}
