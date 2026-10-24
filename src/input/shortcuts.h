#ifndef FLAME_INPUT_SHORTCUTS_H
#define FLAME_INPUT_SHORTCUTS_H

#include <stdbool.h>
#include <wayland-server-core.h>

struct flame_server;

// an app taking every key, flame's shortcuts too, while it has the keyboard
// (keyboard-shortcuts-inhibit: a virtual machine, a remote desktop: super
// goes to the machine in there). super+escape always gets them back, till
// the app's window has the keyboard again

struct flame_shortcuts {
	struct flame_server *server;
	struct wlr_keyboard_shortcuts_inhibit_manager_v1 *manager;
	struct wlr_keyboard_shortcuts_inhibitor_v1 *active; // NULL: flame's shortcuts work
	struct wlr_keyboard_shortcuts_inhibitor_v1 *escaped; // let go of with super+escape

	struct wl_listener new_inhibitor;
	struct wl_listener focus_change;
	struct wl_listener active_destroy;
	struct wl_listener escaped_destroy;
};

bool shortcuts_init(struct flame_shortcuts *shortcuts, struct flame_server *server);
void shortcuts_finish(struct flame_shortcuts *shortcuts);

// an app has flame's shortcuts now
bool shortcuts_inhibited(const struct flame_shortcuts *shortcuts);
// super+escape: flame has them back
void shortcuts_escape(struct flame_shortcuts *shortcuts);

#endif
