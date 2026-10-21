#ifndef FLAME_CAPTURE_CAPTURE_H
#define FLAME_CAPTURE_CAPTURE_H

#include <stdbool.h>
#include <wayland-server-core.h>

struct flame_server;
struct flame_view;

// sharing one window: every window on the ext-foreign-toplevel list, and a
// capture source per window for ext-image-copy-capture (what the screen
// sharing portal asks for when you pick a window). a window is drawn on its
// own, whatever covers it or wherever it is, even on a hidden workspace
struct flame_capture {
	struct flame_server *server;
	struct wlr_ext_foreign_toplevel_list_v1 *list;
	struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1 *sources;
	struct wl_list windows; // capture_window.link, one a mapped view

	struct wl_listener desktop_changed;
	struct wl_listener new_request;
	// something drew: anyone waiting on a window looks right away
	struct wl_listener scene_commit, scene_changed;
	struct wl_event_source *poke;
};

bool capture_init(struct flame_capture *capture, struct flame_server *server);
void capture_finish(struct flame_capture *capture);

// view is going away: its handle and anyone capturing it let go now, not
// at the next desktop change
void capture_view_gone(struct flame_capture *capture, struct flame_view *view);
// view's ext-foreign-toplevel identifier, what the portal names it by. NULL
// when it isn't listed (yet)
const char *capture_identifier(struct flame_capture *capture, struct flame_view *view);

#endif
