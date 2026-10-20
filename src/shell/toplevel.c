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

static struct flame_toplevel *x_focused_popup(struct flame_toplevel *toplevel);

static void impl_set_activated(struct flame_view *view, bool activated) {
	struct flame_toplevel *toplevel = from_view(view);
	// one of our menus has X focus: on X11 we'd have had a FocusOut, and the
	// app stops reacting to hover behind its menu
	if (x_focused_popup(toplevel)) {
		activated = false;
	}
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
	struct flame_toplevel *toplevel = from_view(view);
	// a bridged app titles its X window, the wayland one stays blank
	const struct flame_xbridge_verdict *verdict = xbridge_window_verdict(&toplevel->bridge);
	if (verdict && verdict->title) {
		return verdict->title;
	}
	return toplevel->xdg->title;
}

static void impl_placed(struct flame_view *view, struct wlr_box content) {
	xbridge_window_placed(&from_view(view)->bridge, content);
}

// the window of our bridged process that has X focus, if it's shown as one
// of our menus or glued inside us
static struct flame_toplevel *x_focused_part(struct flame_toplevel *toplevel) {
	struct flame_xbridge_window *window = xbridge_window_x_focus(&toplevel->bridge);
	if (!window) {
		return NULL;
	}
	struct flame_toplevel *focused = wl_container_of(window, focused, bridge);
	bool ours = focused->content_in == FLAME_CONTENT_POPUP
		|| (focused->content_in == FLAME_CONTENT_EMBED && focused->host == toplevel);
	return ours && focused->xdg->base->surface->mapped ? focused : NULL;
}

static struct flame_toplevel *x_focused_popup(struct flame_toplevel *toplevel) {
	struct flame_toplevel *focused = x_focused_part(toplevel);
	return focused && focused->content_in == FLAME_CONTENT_POPUP ? focused : NULL;
}

// keys go where the app put X focus (its open menu, its store page), else
// to whatever inside us was clicked last
static struct wlr_surface *impl_keyboard_surface(struct flame_view *view) {
	struct flame_toplevel *toplevel = from_view(view);
	struct flame_toplevel *focused = x_focused_part(toplevel);
	if (focused) {
		return focused->xdg->base->surface;
	}
	return toplevel->typing ? toplevel->typing->xdg->base->surface : NULL;
}

// a click on something glued inside us hands it the keyboard, a click on
// our own content takes it back
static void impl_pressed(struct flame_view *view, struct wlr_surface *surface) {
	struct flame_toplevel *toplevel = from_view(view);
	struct wlr_surface *root = surface ? wlr_surface_get_root_surface(surface) : NULL;
	toplevel->typing = NULL;
	struct flame_toplevel *embed;
	wl_list_for_each(embed, &toplevel->embeds, embed_link) {
		if (embed->xdg->base->surface == root) {
			toplevel->typing = embed;
			break;
		}
	}
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
	.placed = impl_placed,
	.keyboard_surface = impl_keyboard_surface,
	.pressed = impl_pressed,
};

// ---- bridged windows ----

// its content back home in view.tree, from floating or from its host
static void content_restore(struct flame_toplevel *toplevel) {
	if (toplevel->content_in == FLAME_CONTENT_OWN) {
		return;
	}
	struct flame_toplevel *host = toplevel->host;
	if (host) {
		wl_list_remove(&toplevel->embed_link);
		wl_list_init(&toplevel->embed_link);
		toplevel->host = NULL;
		if (host->typing == toplevel) {
			// the keys go back to the window itself
			host->typing = NULL;
			struct flame_desktop *desktop = host->view.desktop;
			if (desktop->focused == &host->view) {
				focus_restore_keyboard(desktop);
			}
		}
	}
	toplevel->content_in = FLAME_CONTENT_OWN;
	scene_node_reparent(&toplevel->view.content->node, toplevel->view.tree);
	view_content_changed(&toplevel->view);
}

// content out of its own tile into parent at box, sized to box
static void content_move(struct flame_toplevel *toplevel, struct flame_tree *parent,
		struct wlr_box box) {
	struct flame_view *view = &toplevel->view;
	if (view->content->node.parent != parent) {
		scene_node_reparent(&view->content->node, parent);
	}
	scene_node_set_position(&view->content->node, box.x, box.y);
	if (box.width > 0 && box.height > 0) {
		impl_configure(view, box.width, box.height, false);
		impl_set_display_size(view, box.width, box.height);
	}
}

// the window an embed goes inside, if it can hold one right now: on screen in
// its own tile
static struct flame_toplevel *embed_host(struct flame_toplevel *toplevel, uint32_t index) {
	struct flame_xbridge_window *window = xbridge_window_sibling(&toplevel->bridge, index);
	if (!window) {
		return NULL;
	}
	struct flame_toplevel *host = wl_container_of(window, host, bridge);
	if (host->dying || !host->xdg->base->surface->mapped
			|| host->content_in != FLAME_CONTENT_OWN || !host->view.workspace) {
		return NULL;
	}
	return host;
}

// map, or the X side changed its mind: tile it, float it, glue it inside
// another window, or keep it away
static void bridge_apply(struct flame_xbridge_window *window) {
	struct flame_toplevel *toplevel = wl_container_of(window, toplevel, bridge);
	struct flame_view *view = &toplevel->view;
	struct flame_desktop *desktop = view->desktop;
	const struct flame_xbridge_verdict *verdict = xbridge_window_verdict(window);
	// not bridged (anymore) is a plain window
	enum flame_xbridge_role role = verdict ? verdict->role : FLAME_XBRIDGE_WINDOW;
	bool mapped = toplevel->xdg->base->surface->mapped && !toplevel->dying;
	bool was_popup = toplevel->content_in == FLAME_CONTENT_POPUP;
	if (verdict) {
		wlr_log(WLR_DEBUG, "xbridge: #%u is %s %d,%d %dx%d%s%s", window->index,
			(const char *[]){"hidden", "window", "popup", "embed"}[verdict->role],
			verdict->box.x, verdict->box.y, verdict->box.width, verdict->box.height,
			verdict->grabs ? " (holds the pointer)" : "", mapped ? "" : " (not mapped yet)");
	}
	struct flame_toplevel *host =
		role == FLAME_XBRIDGE_EMBED ? embed_host(toplevel, verdict->host) : NULL;
	desktop_notify(desktop); // the title may be new

	if (!mapped || role != FLAME_XBRIDGE_POPUP) {
		if (toplevel->content_in == FLAME_CONTENT_POPUP) {
			content_restore(toplevel);
		}
	}
	if (!mapped || role != FLAME_XBRIDGE_EMBED || host != toplevel->host) {
		if (toplevel->content_in == FLAME_CONTENT_EMBED) {
			content_restore(toplevel);
		}
	}
	if (!mapped) {
		return; // map comes back here
	}

	switch (role) {
	case FLAME_XBRIDGE_HIDDEN:
		desktop_unmap_view(desktop, view);
		break;
	case FLAME_XBRIDGE_WINDOW:
		if (!view->workspace) {
			desktop_map_view(desktop, view);
		}
		break;
	case FLAME_XBRIDGE_POPUP:
		// a menu sits exactly where the app put it, no tile, no border, above all
		desktop_unmap_view(desktop, view);
		content_move(toplevel, desktop->server->render.layers[FLAME_LAYER_OVERLAY],
			verdict->box);
		toplevel->content_in = FLAME_CONTENT_POPUP;
		// active while it has X focus, the window behind it isn't
		if (toplevel->xdg->base->initialized) {
			wlr_xdg_toplevel_set_activated(toplevel->xdg, xbridge_window_x_focused(window));
		}
		break;
	case FLAME_XBRIDGE_EMBED:
		// over its host like on X: moves, hides and stacks with it
		desktop_unmap_view(desktop, view);
		if (host) {
			content_move(toplevel, host->view.content, verdict->box);
			if (toplevel->content_in != FLAME_CONTENT_EMBED) {
				toplevel->content_in = FLAME_CONTENT_EMBED;
				toplevel->host = host;
				wl_list_insert(&host->embeds, &toplevel->embed_link);
			}
		}
		break;
	}
	// a window that just showed up or went away takes its embeds along. only
	// tiles hold embeds, so an embed never recurses into another
	if (role != FLAME_XBRIDGE_EMBED) {
		xbridge_window_embeds_changed(window);
	}
	// a menu opening or closing moves activation and keys between it and
	// the window behind it
	if (was_popup != (toplevel->content_in == FLAME_CONTENT_POPUP) && desktop->focused) {
		view_set_focused(desktop->focused, true);
		focus_restore_keyboard(desktop);
	}
}

static void handle_map(struct wl_listener *listener, void *data) {
	struct flame_toplevel *toplevel = wl_container_of(listener, toplevel, map);
	wlr_log(WLR_DEBUG, "map toplevel app_id=%s title=%s",
		toplevel->xdg->app_id ? toplevel->xdg->app_id : "?",
		toplevel->xdg->title ? toplevel->xdg->title : "?");
	bridge_apply(&toplevel->bridge);
}

static void handle_unmap(struct wl_listener *listener, void *data) {
	struct flame_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);
	content_restore(toplevel);
	desktop_unmap_view(toplevel->view.desktop, &toplevel->view);
	// whatever was glued inside it can't stay there
	xbridge_window_embeds_changed(&toplevel->bridge);
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

	// embeds out of our tree first, it goes down with view.tree
	toplevel->dying = true;
	xbridge_window_embeds_changed(&toplevel->bridge);
	xbridge_window_detach(&toplevel->bridge);
	content_restore(toplevel);
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
	wl_list_init(&toplevel->embeds);
	wl_list_init(&toplevel->embed_link);
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
	// counted in creation order, the same order the bridge sees its X windows
	xbridge_window_attach(&server->xbridge, &toplevel->bridge,
		xdg_toplevel->base->client->client, &toplevel->view, bridge_apply);
}
