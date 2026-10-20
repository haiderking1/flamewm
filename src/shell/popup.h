#ifndef FLAME_SHELL_POPUP_H
#define FLAME_SHELL_POPUP_H

#include <wayland-server-core.h>

struct flame_server;
struct wlr_xdg_popup;

struct flame_popup {
	struct flame_server *server;
	struct wlr_xdg_popup *xdg;

	struct wl_listener commit;
	struct wl_listener reposition;
	struct wl_listener destroy;
};

// frees itself when the client destroys the popup
void popup_create(struct flame_server *server, struct wlr_xdg_popup *xdg_popup);

#endif
