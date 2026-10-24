#include "desktop/floating.h"

#include "config/config.h"
#include "core/server.h"
#include "util/macros.h"

#define MIN_FLOAT_SIZE 40

static struct wlr_box usable_area(struct flame_desktop *desktop) {
	struct flame_output *output = outputs_primary(&desktop->server->outputs);
	return output ? output->usable : (struct wlr_box){0, 0, 1280, 720};
}

// no bigger than the output, no smaller than something you can grab
static struct wlr_box fit(struct flame_desktop *desktop, struct wlr_box box) {
	struct wlr_box area = usable_area(desktop);
	box.width = CLAMP(box.width, MIN_FLOAT_SIZE, MAX(area.width, MIN_FLOAT_SIZE));
	box.height = CLAMP(box.height, MIN_FLOAT_SIZE, MAX(area.height, MIN_FLOAT_SIZE));
	return box;
}

static struct wlr_box centered(struct flame_desktop *desktop, int width, int height) {
	struct wlr_box area = usable_area(desktop);
	struct wlr_box box = fit(desktop, (struct wlr_box){0, 0, width, height});
	box.x = area.x + (area.width - box.width) / 2;
	box.y = area.y + (area.height - box.height) / 2;
	return box;
}

struct wlr_box floating_on_screen(struct flame_desktop *desktop, struct wlr_box box) {
	// at least a grabbable corner stays on the output
	struct wlr_box area = usable_area(desktop);
	box = fit(desktop, box);
	box.x = CLAMP(box.x, area.x - box.width + MIN_FLOAT_SIZE,
		area.x + area.width - MIN_FLOAT_SIZE);
	box.y = CLAMP(box.y, area.y, area.y + area.height - MIN_FLOAT_SIZE);
	return box;
}

struct wlr_box floating_initial_box(struct flame_desktop *desktop, struct flame_view *view) {
	if (view->has_float_box) {
		return fit(desktop, view->float_box);
	}
	int width = 0, height = 0;
	if (view->impl->natural_size) {
		view->impl->natural_size(view, &width, &height);
	} else {
		view->impl->content_size(view, &width, &height);
	}
	struct wlr_box area = usable_area(desktop);
	if (width <= 1 || height <= 1) {
		// nothing drawn yet and no size wished for: half the screen
		width = area.width / 2;
		height = area.height / 2;
	}
	int bw = view_border_width(view, false);
	return centered(desktop, width + 2 * bw, height + 2 * bw);
}

void floating_toggle(struct flame_desktop *desktop, struct flame_view *view) {
	struct flame_workspace *workspace = view->workspace;
	if (!workspace || view->fullscreen_wanted) {
		return;
	}
	drag_forget(&desktop->server->input.drag, view);
	drag_tree_changed(&desktop->server->input.drag);
	transaction_flush(&desktop->transaction);
	view->float_auto = false; // the user's now
	if (view->floating) {
		workspace_remove_view(workspace, view);
		view->floating = false;
		if (!workspace_add_view(workspace, view)) {
			view->floating = true;
			workspace_add_float(workspace, view);
		}
	} else {
		struct wlr_box was = view->geometry;
		workspace_remove_view(workspace, view);
		view->floating = true;
		if (!view->has_float_box) {
			// its tile's size, a bit smaller, in the middle
			view->float_box = centered(desktop, was.width * 4 / 5, was.height * 4 / 5);
			view->has_float_box = true;
		}
		workspace_add_float(workspace, view);
	}
	workspace->last_focused = view;
	desktop_arrange(desktop);
	focus_view(desktop, view);
	desktop_notify(desktop);
}

void floating_fixed(struct flame_desktop *desktop, struct flame_view *view, bool fixed,
		int width, int height) {
	struct flame_workspace *workspace = view->workspace;
	if (!workspace || view->fullscreen_wanted || fixed == view->floating
			|| (!fixed && !view->float_auto)) {
		return;
	}
	drag_forget(&desktop->server->input.drag, view);
	drag_tree_changed(&desktop->server->input.drag);
	transaction_flush(&desktop->transaction);
	workspace_remove_view(workspace, view);
	if (fixed) {
		int bw = view_border_width(view, false);
		view->float_box = centered(desktop, width + 2 * bw, height + 2 * bw);
		view->has_float_box = true;
		view->floating = true;
		view->float_auto = true;
		workspace_add_float(workspace, view);
	} else {
		view->floating = false;
		view->float_auto = false;
		view->has_float_box = false; // a float of its own later starts over
		if (!workspace_add_view(workspace, view)) {
			view->floating = true;
			workspace_add_float(workspace, view);
		}
	}
	if (workspace == desktop->active) {
		desktop_arrange(desktop);
	}
	desktop_notify(desktop);
}

void floating_set_box(struct flame_desktop *desktop, struct flame_view *view,
		struct wlr_box box, bool live) {
	if (!view->floating) {
		return;
	}
	view->float_box = fit(desktop, box);
	view->has_float_box = true;
	if (!live) {
		desktop_arrange(desktop);
		return;
	}
	// a mouse resize: shown with what it last drew, the client catches up
	view_set_resizing(view, true);
	view_configure(view, view->float_box, false);
	view_place(view, view->float_box, false);
}

void floating_content_resized(struct flame_desktop *desktop, struct flame_view *view,
		int width, int height) {
	if (!view->floating || view->fullscreen_shown || view->resizing || view->grabbed) {
		return;
	}
	int bw = view_border_width(view, false);
	struct wlr_box box = view->float_box;
	if (box.width == width + 2 * bw && box.height == height + 2 * bw) {
		return;
	}
	// it grows or shrinks around where it is
	box.width = width + 2 * bw;
	box.height = height + 2 * bw;
	floating_set_box(desktop, view, box, false);
}
