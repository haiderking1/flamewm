#include "shell/xdg.h"

#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "core/server.h"
#include "shell/popup.h"
#include "shell/toplevel.h"
#include "util/listener.h"

static void handle_new_toplevel(struct wl_listener *listener, void *data) {
	struct flame_xdg *xdg = wl_container_of(listener, xdg, new_toplevel);
	toplevel_create(xdg->server, data);
}

static void handle_new_popup(struct wl_listener *listener, void *data) {
	struct flame_xdg *xdg = wl_container_of(listener, xdg, new_popup);
	struct wlr_xdg_popup *popup = data;

	// layer surface popups get their parent later, layers/ picks those up
	struct wlr_xdg_surface *parent = popup->parent
		? wlr_xdg_surface_try_from_wlr_surface(popup->parent)
		: NULL;
	if (parent && parent->data) {
		popup_create(xdg->server, popup, parent->data);
	}
}

bool xdg_init(struct flame_xdg *xdg, struct flame_server *server) {
	xdg->server = server;
	// v6 for wm_capabilities + suspended
	xdg->shell = wlr_xdg_shell_create(server->display, 6);
	if (!xdg->shell) {
		wlr_log(WLR_ERROR, "failed to create xdg-shell");
		return false;
	}

	if (!decorations_init(&xdg->decorations, server->display)) {
		return false;
	}

	listener_connect(&xdg->new_toplevel, &xdg->shell->events.new_toplevel,
		handle_new_toplevel);
	listener_connect(&xdg->new_popup, &xdg->shell->events.new_popup, handle_new_popup);
	return true;
}

void xdg_finish(struct flame_xdg *xdg) {
	listener_disconnect(&xdg->new_toplevel);
	listener_disconnect(&xdg->new_popup);
	decorations_finish(&xdg->decorations);
}
