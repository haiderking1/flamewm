#ifndef FLAME_LOCK_LOCK_H
#define FLAME_LOCK_LOCK_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

#include "lock/sleep.h"

// the session locked (ext-session-lock-v1): a locker's surfaces over
// everything, one per screen, an opaque cover under and around them, and
// nothing else gets a key or the pointer: no window, no X app, no bind.
// the locker's told it's locked once every screen has shown it. one that
// goes away without unlocking leaves it locked, and another can take over

struct flame_server;
struct flame_tree;
struct flame_rect;
struct wlr_surface;
struct wlr_session_lock_manager_v1;
struct wlr_session_lock_v1;

struct flame_lock {
	struct flame_server *server;
	struct wlr_session_lock_manager_v1 *manager;
	struct wlr_session_lock_v1 *lock; // the locker's, NULL while there's none
	bool told; // the locker knows it's locked
	bool locked; // till the locker unlocks, whatever happens to it
	struct flame_tree *tree; // over every other layer
	struct flame_rect *cover; // the whole layout, under the locker's surfaces
	// the wallpaper over the cover while there's a locker: its surfaces
	// show it through their glass. gone with the locker, just the cover
	struct flame_tree *backdrop;
	struct wl_list surfaces; // struct lock_surface.link
	struct wl_list waits; // screens still to show a locked frame, struct lock_wait.link
	struct flame_sleep sleep; // locked before the computer sleeps

	struct wl_listener new_lock;
	struct wl_listener unlock;
	struct wl_listener lock_destroy;
	struct wl_listener new_surface;
	struct wl_listener layout_change;
};

bool lock_init(struct flame_lock *lock, struct flame_server *server);
void lock_finish(struct flame_lock *lock);

bool lock_active(const struct flame_lock *lock);
// may surface have the keyboard: anything while unlocked, only the
// locker's while locked
bool lock_allows(const struct flame_lock *lock, struct wlr_surface *surface);
// the pointer at lx, ly while locked: the locker's surface under it gets it,
// a press there the keyboard too. pressed: a button went down
void lock_pointer(struct flame_lock *lock, double lx, double ly, uint32_t time_msec,
	bool pressed);

#endif
