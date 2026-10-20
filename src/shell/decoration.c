#include "shell/decoration.h"

#include <stdlib.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "util/listener.h"

struct flame_decoration {
	struct wlr_xdg_toplevel_decoration_v1 *wlr;

	struct wl_listener request_mode;
	struct wl_listener commit;
	struct wl_listener destroy;
};

static void force_server_side(struct flame_decoration *decoration) {
	// set_mode sends a configure, which only works once the surface is ready.
	// before that the initial commit picks it up
	if (decoration->wlr->toplevel->base->initialized) {
		wlr_xdg_toplevel_decoration_v1_set_mode(decoration->wlr,
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
}

static void handle_request_mode(struct wl_listener *listener, void *data) {
	struct flame_decoration *decoration = wl_container_of(listener, decoration, request_mode);
	force_server_side(decoration);
}

static void handle_commit(struct wl_listener *listener, void *data) {
	struct flame_decoration *decoration = wl_container_of(listener, decoration, commit);
	if (decoration->wlr->toplevel->base->initial_commit) {
		force_server_side(decoration);
	}
}

static void handle_destroy(struct wl_listener *listener, void *data) {
	struct flame_decoration *decoration = wl_container_of(listener, decoration, destroy);
	listener_disconnect(&decoration->request_mode);
	listener_disconnect(&decoration->commit);
	listener_disconnect(&decoration->destroy);
	free(decoration);
}

static void handle_new_decoration(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_decoration_v1 *wlr_decoration = data;

	struct flame_decoration *decoration = calloc(1, sizeof(*decoration));
	if (!decoration) {
		wlr_log(WLR_ERROR, "out of memory");
		return;
	}
	decoration->wlr = wlr_decoration;

	listener_connect(&decoration->request_mode, &wlr_decoration->events.request_mode,
		handle_request_mode);
	listener_connect(&decoration->commit,
		&wlr_decoration->toplevel->base->surface->events.commit, handle_commit);
	listener_connect(&decoration->destroy, &wlr_decoration->events.destroy,
		handle_destroy);

	force_server_side(decoration);
}

bool decorations_init(struct flame_decorations *decorations, struct wl_display *display) {
	decorations->manager = wlr_xdg_decoration_manager_v1_create(display);
	if (!decorations->manager) {
		wlr_log(WLR_ERROR, "failed to create xdg-decoration manager");
		return false;
	}
	listener_connect(&decorations->new_decoration,
		&decorations->manager->events.new_toplevel_decoration, handle_new_decoration);
	return true;
}

void decorations_finish(struct flame_decorations *decorations) {
	listener_disconnect(&decorations->new_decoration);
}
