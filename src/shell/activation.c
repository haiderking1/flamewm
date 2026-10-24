#include "shell/activation.h"

#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/util/log.h>

#include "core/server.h"
#include "desktop/focus.h"
#include "util/listener.h"

// the window surface is, on any workspace
static struct flame_view *view_of(struct flame_desktop *desktop, struct wlr_surface *surface) {
	for (int i = 0; i < FLAME_WORKSPACE_COUNT; i++) {
		struct wl_list *lists[2] = {&desktop->workspaces[i].views,
			&desktop->workspaces[i].floats};
		for (int l = 0; l < 2; l++) {
			struct flame_view *view;
			wl_list_for_each(view, lists[l], link) {
				if (view->impl->surface && view->impl->surface(view) == surface) {
					return view;
				}
			}
		}
	}
	return NULL;
}

static void handle_request_activate(struct wl_listener *listener, void *data) {
	struct flame_activation *activation =
		wl_container_of(listener, activation, request_activate);
	struct wlr_xdg_activation_v1_request_activate_event *event = data;
	struct flame_desktop *desktop = &activation->server->desktop;
	struct flame_view *view = view_of(desktop, wlr_surface_get_root_surface(event->surface));
	if (!view || !view->workspace) {
		return;
	}
	if (!event->token->seat) {
		wlr_log(WLR_DEBUG, "activation: %s asks to come up on its own: not now",
			view_app_id(view) ? view_app_id(view) : "?");
		return;
	}
	wlr_log(WLR_DEBUG, "activation: %s comes up", view_app_id(view) ? view_app_id(view) : "?");
	if (view->workspace != desktop->active) {
		desktop_switch_workspace(desktop, (int)(view->workspace - desktop->workspaces));
	}
	focus_view(desktop, view);
}

bool activation_init(struct flame_activation *activation, struct flame_server *server) {
	activation->server = server;
	activation->wlr = wlr_xdg_activation_v1_create(server->display);
	if (!activation->wlr) {
		wlr_log(WLR_ERROR, "failed to create xdg-activation");
		return false;
	}
	listener_connect(&activation->request_activate, &activation->wlr->events.request_activate,
		handle_request_activate);
	return true;
}

void activation_finish(struct flame_activation *activation) {
	listener_disconnect(&activation->request_activate);
}
