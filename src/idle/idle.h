#ifndef FLAME_IDLE_IDLE_H
#define FLAME_IDLE_IDLE_H

#include <stdbool.h>
#include <wayland-server-core.h>

struct flame_server;

// the user being away: apps that want to know when nobody's touched
// anything for a while hear it (ext-idle-notify: swayidle, hypridle locking
// or blanking the screen), and none of it while something keeps the screen
// on: a video or a game asking (idle-inhibit) while it's on screen, an X
// player suspending the screen saver

struct flame_idle {
	struct flame_server *server;
	struct wlr_idle_notifier_v1 *notifier;
	struct wlr_idle_inhibit_manager_v1 *inhibit_manager;
	struct wl_list inhibitors; // idle_inhibitor.link
	bool x_suspended; // an X app keeps the screen on
	bool inhibited; // as last told
	// while there are inhibitors: whether they're on screen, looked at again
	struct wl_event_source *recheck;

	struct wl_listener new_inhibitor;
	struct wl_listener desktop_changed;
};

bool idle_init(struct flame_idle *idle, struct flame_server *server);
void idle_finish(struct flame_idle *idle);

// the user did something: a key, the mouse
void idle_activity(struct flame_idle *idle);
// an X app keeps the screen on, or stopped (MIT-SCREEN-SAVER's Suspend)
void idle_x_suspended(struct flame_idle *idle, bool suspended);

#endif
