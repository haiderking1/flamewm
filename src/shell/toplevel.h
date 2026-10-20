#ifndef FLAME_SHELL_TOPLEVEL_H
#define FLAME_SHELL_TOPLEVEL_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wlr/util/box.h>

#include "desktop/view.h"
#include "xbridge/xbridge.h"

struct flame_server;
struct flame_xdg_tree;
struct wlr_xdg_toplevel;

struct flame_toplevel {
	struct flame_view view;
	struct wlr_xdg_toplevel *xdg;
	struct flame_xdg_tree *xdg_tree; // the xdg surface's scene tree inside view.content

	// what we last asked for
	int configured_width, configured_height;
	bool configured_fullscreen;
	uint32_t configure_serial;
	// already re-sent once for this size because the client ignored it
	bool corrected;

	// what we're showing, the client can't draw past it
	int display_width, display_height;
	struct wlr_box clip; // last clip applied, in surface coords

	// steam's webhelper and friends: the X side says what this window is
	struct flame_xbridge_window bridge;
	// where a bridged window's content is, when it's not in its own view.tree
	enum {
		FLAME_CONTENT_OWN,
		FLAME_CONTENT_POPUP, // floating on the overlay layer
		FLAME_CONTENT_EMBED, // inside host's content
	} content_in;
	struct flame_toplevel *host; // embedded: the window it's inside
	struct wl_list embeds; // flame_toplevel.embed_link, the ones inside this one
	struct wl_list embed_link;
	// the embedded one that was clicked last types instead of us
	struct flame_toplevel *typing;
	// on its way out: whatever is inside has to leave view.tree first
	bool dying;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener client_commit; // before a commit applies: a null buffer coming
	struct wl_listener destroy;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
	struct wl_listener set_title;
	struct wl_listener set_app_id;
};

// frees itself when the client destroys the toplevel
void toplevel_create(struct flame_server *server, struct wlr_xdg_toplevel *xdg_toplevel);

#endif
