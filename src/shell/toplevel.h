#ifndef FLAME_SHELL_TOPLEVEL_H
#define FLAME_SHELL_TOPLEVEL_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wlr/util/box.h>

#include "desktop/view.h"

struct flame_server;
struct wlr_xdg_toplevel;

struct flame_toplevel {
	struct flame_view view;
	struct wlr_xdg_toplevel *xdg;
	struct wlr_scene_tree *scene_tree; // the xdg surface tree inside view.content

	// what we last asked for
	int configured_width, configured_height;
	bool configured_fullscreen;
	uint32_t configure_serial;
	// already re-sent once for this size because the client ignored it
	bool corrected;

	// what we're showing, the client can't draw past it
	int display_width, display_height;
	struct wlr_box clip; // last clip applied, in surface coords

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
	struct wl_listener set_title;
	struct wl_listener set_app_id;
};

// frees itself when the client destroys the toplevel
void toplevel_create(struct flame_server *server, struct wlr_xdg_toplevel *xdg_toplevel);

#endif
