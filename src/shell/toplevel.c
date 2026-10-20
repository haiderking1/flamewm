#include "shell/toplevel.h"

#include <stdlib.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>

#include "core/server.h"
#include "util/listener.h"

static struct flame_toplevel *from_view(struct flame_view *view) {
	struct flame_toplevel *toplevel = wl_container_of(view, toplevel, view);
	return toplevel;
}

// keeps csd shadows or a client that ignores its size from drawing over
// borders and neighbours. surface coords, so offset by the window geometry
static void update_clip(struct flame_toplevel *toplevel) {
	struct wlr_box geometry = toplevel->xdg->base->geometry;
	struct wlr_box clip = {
		.x = geometry.x,
		.y = geometry.y,
		.width = toplevel->display_width,
		.height = toplevel->display_height,
	};
	if (wlr_box_equal(&clip, &toplevel->clip)) {
		return;
	}
	toplevel->clip = clip;
	wlr_scene_subsurface_tree_set_clip(&toplevel->scene_tree->node, &clip);
}

static uint32_t send_size(struct flame_toplevel *toplevel, int width, int height) {
	toplevel->configured_width = width;
	toplevel->configured_height = height;
	toplevel->corrected = false;
	toplevel->configure_serial = wlr_xdg_toplevel_set_size(toplevel->xdg, width, height);
	return toplevel->configure_serial;
}

static uint32_t impl_configure(struct flame_view *view, int width, int height) {
	struct flame_toplevel *toplevel = from_view(view);
	if (!toplevel->xdg->base->initialized) {
		return 0; // the initial commit sends the first one
	}
	if (width == toplevel->configured_width && height == toplevel->configured_height) {
		return 0;
	}
	return send_size(toplevel, width, height);
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

static void impl_set_activated(struct flame_view *view, bool activated) {
	struct flame_toplevel *toplevel = from_view(view);
	if (toplevel->xdg->base->initialized) {
		wlr_xdg_toplevel_set_activated(toplevel->xdg, activated);
	}
}

static void impl_close(struct flame_view *view) {
	wlr_xdg_toplevel_send_close(from_view(view)->xdg);
}

static struct wlr_surface *impl_surface(struct flame_view *view) {
	return from_view(view)->xdg->base->surface;
}

static const struct flame_view_impl toplevel_impl = {
	.configure = impl_configure,
	.acked = impl_acked,
	.set_display_size = impl_set_display_size,
	.set_activated = impl_set_activated,
	.close = impl_close,
	.surface = impl_surface,
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
}

// first configure already carries the tile this window is going to get,
// so its first buffer is the right size and nothing jumps on map
static void send_initial_configure(struct flame_toplevel *toplevel) {
	struct wlr_xdg_toplevel *xdg = toplevel->xdg;

	// no maximize/fullscreen/minimize buttons, and tell it it's tiled so
	// csd apps drop shadows and rounded corners
	wlr_xdg_toplevel_set_wm_capabilities(xdg, 0);
	wlr_xdg_toplevel_set_tiled(xdg, WLR_EDGE_TOP | WLR_EDGE_BOTTOM
		| WLR_EDGE_LEFT | WLR_EDGE_RIGHT);

	struct wlr_box tile;
	if (!desktop_prospective_geometry(toplevel->view.desktop, &tile)) {
		send_size(toplevel, 0, 0); // no output yet, let it pick
		return;
	}
	int width, height;
	view_content_size(&toplevel->view, tile, &width, &height);
	wlr_xdg_toplevel_set_bounds(xdg, width, height);
	send_size(toplevel, width, height);
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
	if (toplevel->xdg->base->initial_commit) {
		send_initial_configure(toplevel);
		return;
	}
	// window geometry can move with any commit
	update_clip(toplevel);
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
	if (toplevel->xdg->base->initialized) {
		wlr_xdg_surface_schedule_configure(toplevel->xdg->base);
	}
}

static void handle_destroy(struct wl_listener *listener, void *data) {
	struct flame_toplevel *toplevel = wl_container_of(listener, toplevel, destroy);

	listener_disconnect(&toplevel->map);
	listener_disconnect(&toplevel->unmap);
	listener_disconnect(&toplevel->commit);
	listener_disconnect(&toplevel->destroy);
	listener_disconnect(&toplevel->request_maximize);
	listener_disconnect(&toplevel->request_fullscreen);

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
	toplevel->scene_tree =
		wlr_scene_xdg_surface_create(toplevel->view.content, xdg_toplevel->base);
	xdg_toplevel->base->data = toplevel->scene_tree;

	struct wlr_surface *surface = xdg_toplevel->base->surface;
	listener_connect(&toplevel->map, &surface->events.map, handle_map);
	listener_connect(&toplevel->unmap, &surface->events.unmap, handle_unmap);
	listener_connect(&toplevel->commit, &surface->events.commit, handle_commit);
	listener_connect(&toplevel->destroy, &xdg_toplevel->events.destroy, handle_destroy);
	listener_connect(&toplevel->request_maximize, &xdg_toplevel->events.request_maximize,
		handle_request_maximize);
	listener_connect(&toplevel->request_fullscreen,
		&xdg_toplevel->events.request_fullscreen, handle_request_fullscreen);
}
