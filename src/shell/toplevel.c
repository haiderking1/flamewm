#include "shell/toplevel.h"

#include <stdlib.h>
#include <strings.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>

#include "core/server.h"
#include "shell/xdg_tree.h"
#include "util/listener.h"
#include "util/macros.h"

static struct flame_toplevel *from_view(struct flame_view *view) {
	struct flame_toplevel *toplevel = wl_container_of(view, toplevel, view);
	return toplevel;
}

static bool is_firefox(struct wlr_xdg_toplevel *xdg) {
	static const char *const ids[] = {
		"firefox", "firefox-esr", "org.mozilla.firefox", "org.mozilla.firefox_esr",
	};
	if (!xdg->app_id) {
		return false;
	}
	for (size_t i = 0; i < LENGTH(ids); i++) {
		if (!strcasecmp(xdg->app_id, ids[i])) {
			return true;
		}
	}
	return false;
}

// firefox's gtk puts its csd shadow margins back after leaving fullscreen
// even with all four tiled edges set, and those margins show up as a strip.
// maximized makes gtk drop them for good. raven 57787f2
static bool wants_maximized_hint(struct wlr_xdg_toplevel *xdg) {
	return is_firefox(xdg);
}

// keeps csd shadows, an oversized buffer firefox attaches before acking
// fullscreen, or a client that ignores its size from drawing over borders and
// neighbours. window geometry clipped to what we show, in surface coords
static void update_clip(struct flame_toplevel *toplevel) {
	struct wlr_box geometry = toplevel->xdg->base->geometry;
	int width = toplevel->display_width;
	int height = toplevel->display_height;
	if (geometry.width > 0 && geometry.height > 0) {
		width = MIN(width, geometry.width);
		height = MIN(height, geometry.height);
	}
	struct wlr_box clip = {
		.x = geometry.x,
		.y = geometry.y,
		.width = width,
		.height = height,
	};
	if (wlr_box_equal(&clip, &toplevel->clip)) {
		return;
	}
	toplevel->clip = clip;
	scene_surface_set_clip(toplevel->xdg_tree->surface, &clip);
}

// every configure carries the full picture. the tiled edges stay set even in
// fullscreen: flipping them makes gtk redo its csd and shadow geometry in the
// middle of the resize
static uint32_t send_state(struct flame_toplevel *toplevel, int width, int height,
		bool fullscreen) {
	struct wlr_xdg_toplevel *xdg = toplevel->xdg;

	toplevel->configured_width = width;
	toplevel->configured_height = height;
	toplevel->configured_fullscreen = fullscreen;
	toplevel->corrected = false;

	wlr_xdg_toplevel_set_tiled(xdg, WLR_EDGE_TOP | WLR_EDGE_BOTTOM
		| WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
	wlr_xdg_toplevel_set_maximized(xdg, wants_maximized_hint(xdg));
	wlr_xdg_toplevel_set_fullscreen(xdg, fullscreen);
	if (width > 0 && height > 0) {
		wlr_xdg_toplevel_set_bounds(xdg, width, height);
	}
	toplevel->configure_serial = wlr_xdg_toplevel_set_size(xdg, width, height);
	return toplevel->configure_serial;
}

static uint32_t impl_configure(struct flame_view *view, int width, int height,
		bool fullscreen) {
	struct flame_toplevel *toplevel = from_view(view);
	if (!toplevel->xdg->base->initialized) {
		return 0; // the initial commit sends the first one
	}
	if (width == toplevel->configured_width && height == toplevel->configured_height
			&& fullscreen == toplevel->configured_fullscreen) {
		return 0;
	}
	return send_state(toplevel, width, height, fullscreen);
}

static bool impl_acked(struct flame_view *view, uint32_t serial) {
	// wrap safe "acked serial is this one or newer"
	uint32_t acked = from_view(view)->xdg->base->pending.configure_serial;
	return (int32_t)(acked - serial) >= 0;
}

static void impl_set_display_size(struct flame_view *view, int width, int height) {
	struct flame_toplevel *toplevel = from_view(view);
	toplevel->display_width = width;
	toplevel->display_height = height;
	update_clip(toplevel);
}

static void impl_content_size(struct flame_view *view, int *width, int *height) {
	struct flame_toplevel *toplevel = from_view(view);
	struct wlr_box geometry = toplevel->xdg->base->geometry;
	*width = geometry.width > 0 ? MIN(geometry.width, toplevel->display_width)
		: toplevel->display_width;
	*height = geometry.height > 0 ? MIN(geometry.height, toplevel->display_height)
		: toplevel->display_height;
}

static struct wlr_box impl_window_geometry(struct flame_view *view, bool pending) {
	struct wlr_xdg_surface *base = from_view(view)->xdg->base;
	return pending ? base->pending.geometry : base->current.geometry;
}

static void impl_set_activated(struct flame_view *view, bool activated) {
	struct flame_toplevel *toplevel = from_view(view);
	if (toplevel->xdg->base->initialized) {
		wlr_xdg_toplevel_set_activated(toplevel->xdg, activated);
	}
}

static void impl_set_resizing(struct flame_view *view, bool resizing) {
	struct wlr_xdg_toplevel *xdg = from_view(view)->xdg;
	// unmap can end a drag, by then there's nobody to tell
	if (xdg->base->initialized && xdg->base->surface->mapped) {
		wlr_xdg_toplevel_set_resizing(xdg, resizing);
	}
}

static void impl_close(struct flame_view *view) {
	wlr_xdg_toplevel_send_close(from_view(view)->xdg);
}

static struct wlr_surface *impl_surface(struct flame_view *view) {
	return from_view(view)->xdg->base->surface;
}

static const char *impl_app_id(struct flame_view *view) {
	return from_view(view)->xdg->app_id;
}

static const char *impl_title(struct flame_view *view) {
	return from_view(view)->xdg->title;
}

static const struct flame_view_impl toplevel_impl = {
	.configure = impl_configure,
	.acked = impl_acked,
	.set_display_size = impl_set_display_size,
	.content_size = impl_content_size,
	.window_geometry = impl_window_geometry,
	.set_activated = impl_set_activated,
	.set_resizing = impl_set_resizing,
	.close = impl_close,
	.surface = impl_surface,
	.app_id = impl_app_id,
	.title = impl_title,
};

static void handle_map(struct wl_listener *listener, void *data) {
	struct flame_toplevel *toplevel = wl_container_of(listener, toplevel, map);
	wlr_log(WLR_DEBUG, "map toplevel app_id=%s title=%s",
		toplevel->xdg->app_id ? toplevel->xdg->app_id : "?",
		toplevel->xdg->title ? toplevel->xdg->title : "?");
	desktop_map_view(toplevel->view.desktop, &toplevel->view);
}

static void handle_unmap(struct wl_listener *listener, void *data) {
	struct flame_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);
	desktop_unmap_view(toplevel->view.desktop, &toplevel->view);
	// the next mapping starts with a fresh initial configure
	toplevel->configured_width = 0;
	toplevel->configured_height = 0;
	toplevel->configured_fullscreen = false;
}

// configures a window that hasn't mapped yet with exactly what it's going to
// get, so its first buffer is the right size and nothing jumps on map:
// the whole output if it asked for fullscreen, else its future tile
static void configure_unmapped(struct flame_toplevel *toplevel) {
	struct flame_desktop *desktop = toplevel->view.desktop;
	bool fullscreen = toplevel->view.fullscreen_wanted;

	struct wlr_box box;
	bool known = fullscreen ? fullscreen_area(desktop, &box)
		: desktop_prospective_geometry(desktop, &box);
	if (!known) {
		send_state(toplevel, 0, 0, false); // no output yet, let it pick
		return;
	}
	int width, height;
	view_content_size(&toplevel->view, box, fullscreen, &width, &height);
	send_state(toplevel, width, height, fullscreen);
}

// a client can ack our size and still commit another one. ask once more per
// size, never every frame, and leave the rest to the clip
static void reconcile_size(struct flame_toplevel *toplevel) {
	struct wlr_xdg_surface *base = toplevel->xdg->base;
	if (!toplevel->view.shown || transaction_has(&toplevel->view)
			|| toplevel->configured_width <= 0) {
		return;
	}
	if (base->geometry.width == toplevel->configured_width
			&& base->geometry.height == toplevel->configured_height) {
		toplevel->corrected = false;
		return;
	}
	bool settled = base->current.configure_serial == toplevel->configure_serial
		&& wl_list_empty(&base->configure_list) && !base->configure_idle;
	if (!settled || toplevel->corrected) {
		return;
	}
	toplevel->corrected = true;
	wlr_xdg_surface_schedule_configure(base);
}

static void handle_commit(struct wl_listener *listener, void *data) {
	struct flame_toplevel *toplevel = wl_container_of(listener, toplevel, commit);
	struct wlr_xdg_toplevel *xdg = toplevel->xdg;

	if (xdg->base->initial_commit) {
		// fullscreen asked for before the first commit is honored right away
		toplevel->view.fullscreen_wanted = xdg->requested.fullscreen;
		wlr_xdg_toplevel_set_wm_capabilities(xdg, WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN);
		configure_unmapped(toplevel);
		return;
	}
	// window geometry can move with any commit
	update_clip(toplevel);
	if (toplevel->view.shown) {
		view_content_changed(&toplevel->view);
	}
	reconcile_size(toplevel);
}

static void handle_request_maximize(struct wl_listener *listener, void *data) {
	// protocol says we must answer with a configure even if we say no
	struct flame_toplevel *toplevel = wl_container_of(listener, toplevel, request_maximize);
	if (toplevel->xdg->base->initialized) {
		wlr_xdg_surface_schedule_configure(toplevel->xdg->base);
	}
}

static void handle_request_fullscreen(struct wl_listener *listener, void *data) {
	struct flame_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_fullscreen);
	struct wlr_xdg_toplevel *xdg = toplevel->xdg;

	fullscreen_request(toplevel->view.desktop, &toplevel->view, xdg->requested.fullscreen);

	if (!xdg->base->initialized) {
		return; // the initial commit picks it up
	}
	if (!toplevel->view.workspace) {
		// configured but not mapped yet, redo that first configure
		configure_unmapped(toplevel);
		return;
	}
	// always reply, even when refused or the change is still queued behind a
	// transaction. same idle as any configure we just sent, so no extra serial
	wlr_xdg_surface_schedule_configure(xdg->base);
}

// bars show the focused window's title, and browsers change it all the time
static void handle_set_title(struct wl_listener *listener, void *data) {
	struct flame_toplevel *toplevel = wl_container_of(listener, toplevel, set_title);
	desktop_notify(toplevel->view.desktop);
}

static void handle_set_app_id(struct wl_listener *listener, void *data) {
	struct flame_toplevel *toplevel = wl_container_of(listener, toplevel, set_app_id);
	desktop_notify(toplevel->view.desktop);
}

static void handle_destroy(struct wl_listener *listener, void *data) {
	struct flame_toplevel *toplevel = wl_container_of(listener, toplevel, destroy);

	listener_disconnect(&toplevel->map);
	listener_disconnect(&toplevel->unmap);
	listener_disconnect(&toplevel->commit);
	listener_disconnect(&toplevel->destroy);
	listener_disconnect(&toplevel->request_maximize);
	listener_disconnect(&toplevel->request_fullscreen);
	listener_disconnect(&toplevel->set_title);
	listener_disconnect(&toplevel->set_app_id);

	view_finish(&toplevel->view);
	free(toplevel);
}

void toplevel_create(struct flame_server *server, struct wlr_xdg_toplevel *xdg_toplevel) {
	struct flame_toplevel *toplevel = calloc(1, sizeof(*toplevel));
	if (!toplevel) {
		wlr_log(WLR_ERROR, "out of memory");
		return;
	}
	toplevel->xdg = xdg_toplevel;
	view_init(&toplevel->view, &toplevel_impl, &server->desktop);

	// popups find their parent tree through xdg_surface->data
	// popups find their parent's tree through xdg_surface->data
	toplevel->xdg_tree = xdg_tree_create(toplevel->view.content, xdg_toplevel->base);
	if (!toplevel->xdg_tree) {
		wlr_log(WLR_ERROR, "out of memory");
		view_finish(&toplevel->view);
		free(toplevel);
		return;
	}

	struct wlr_surface *surface = xdg_toplevel->base->surface;
	listener_connect(&toplevel->map, &surface->events.map, handle_map);
	listener_connect(&toplevel->unmap, &surface->events.unmap, handle_unmap);
	listener_connect(&toplevel->commit, &surface->events.commit, handle_commit);
	listener_connect(&toplevel->destroy, &xdg_toplevel->events.destroy, handle_destroy);
	listener_connect(&toplevel->request_maximize, &xdg_toplevel->events.request_maximize,
		handle_request_maximize);
	listener_connect(&toplevel->request_fullscreen,
		&xdg_toplevel->events.request_fullscreen, handle_request_fullscreen);
	listener_connect(&toplevel->set_title, &xdg_toplevel->events.set_title, handle_set_title);
	listener_connect(&toplevel->set_app_id, &xdg_toplevel->events.set_app_id,
		handle_set_app_id);
}
