#ifndef FLAME_SHELL_XDG_H
#define FLAME_SHELL_XDG_H

#include <stdbool.h>
#include <wayland-server-core.h>

#include "shell/decoration.h"

struct flame_server;

struct flame_xdg {
	struct flame_server *server;
	struct wlr_xdg_shell *shell;
	struct flame_decorations decorations;

	struct wl_listener new_toplevel;
	struct wl_listener new_popup;
};

bool xdg_init(struct flame_xdg *xdg, struct flame_server *server);
void xdg_finish(struct flame_xdg *xdg);

#endif
