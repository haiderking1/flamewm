#ifndef FLAME_SHELL_ACTIVATION_H
#define FLAME_SHELL_ACTIVATION_H

#include <stdbool.h>
#include <wayland-server-core.h>

struct flame_server;

// a window asking to be brought up (xdg-activation: a link clicked in a
// terminal raises the browser, a notification's "open" its app): it's
// focused, on its workspace, when what asked was something the user did
// (the token carries their click or key). asked out of nowhere: nothing
// moves, no app takes the user's typing

struct flame_activation {
	struct flame_server *server;
	struct wlr_xdg_activation_v1 *wlr;
	struct wl_listener request_activate;
};

bool activation_init(struct flame_activation *activation, struct flame_server *server);
void activation_finish(struct flame_activation *activation);

#endif
