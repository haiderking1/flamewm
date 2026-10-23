#include "lock/lock.h"

#include <stdlib.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/util/log.h>

#include "core/server.h"
#include "scene/scene.h"
#include "util/listener.h"

// a locker's surface on one screen
struct lock_surface {
	struct wl_list link; // flame_lock.surfaces
	struct flame_lock *lock;
	struct wlr_session_lock_surface_v1 *wlr;
	struct flame_tree *tree;
	struct wl_listener map;
	struct wl_listener destroy;
};

// a screen that hasn't shown a frame since it locked
struct lock_wait {
	struct wl_list link; // flame_lock.waits
	struct flame_lock *lock;
	struct wlr_output *output;
	uint32_t after; // a frame committed after this one is a locked one
	struct wl_listener present;
	struct wl_listener destroy;
};

static const float cover_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};

bool lock_active(const struct flame_lock *lock) {
	return lock->locked;
}

bool lock_allows(const struct flame_lock *lock, struct wlr_surface *surface) {
	return !lock->locked || !surface || wlr_session_lock_surface_v1_try_from_wlr_surface(
		wlr_surface_get_root_surface(surface));
}

// ---- telling the locker ----

static void wait_free(struct lock_wait *wait) {
	listener_disconnect(&wait->present);
	listener_disconnect(&wait->destroy);
	wl_list_remove(&wait->link);
	free(wait);
}

static void waits_clear(struct flame_lock *lock) {
	struct lock_wait *wait, *tmp;
	wl_list_for_each_safe(wait, tmp, &lock->waits, link) {
		wait_free(wait);
	}
}

// every screen's shown a locked frame (or there are none): it's locked
static void maybe_locked(struct flame_lock *lock) {
	if (wl_list_empty(&lock->waits) && lock->lock && !lock->told) {
		lock->told = true;
		wlr_session_lock_v1_send_locked(lock->lock);
		wlr_log(WLR_INFO, "lock: locked");
		sleep_locked(&lock->sleep); // going to sleep, it can now
	}
}

static void handle_wait_present(struct wl_listener *listener, void *data) {
	struct lock_wait *wait = wl_container_of(listener, wait, present);
	struct wlr_output_event_present *event = data;
	// one in flight from before the lock doesn't count
	if (!event->presented || event->commit_seq <= wait->after) {
		return;
	}
	struct flame_lock *lock = wait->lock;
	wait_free(wait);
	maybe_locked(lock);
}

static void handle_wait_destroy(struct wl_listener *listener, void *data) {
	struct lock_wait *wait = wl_container_of(listener, wait, destroy);
	struct flame_lock *lock = wait->lock;
	wait_free(wait); // a screen unplugged shows nothing at all
	maybe_locked(lock);
}

static void wait_for_screens(struct flame_lock *lock) {
	waits_clear(lock);
	struct flame_output *output;
	wl_list_for_each(output, &lock->server->outputs.list, link) {
		if (!output->wlr->enabled) {
			continue;
		}
		struct lock_wait *wait = calloc(1, sizeof(*wait));
		if (!wait) {
			continue; // it'll be told without waiting for this one
		}
		wait->lock = lock;
		wait->output = output->wlr;
		wait->after = output->wlr->commit_seq;
		listener_connect(&wait->present, &output->wlr->events.present, handle_wait_present);
		listener_connect(&wait->destroy, &output->wlr->events.destroy, handle_wait_destroy);
		wl_list_insert(&lock->waits, &wait->link);
		wlr_output_schedule_frame(output->wlr);
	}
	maybe_locked(lock);
}

// ---- where things go ----

// the cover over the whole layout, each surface over its screen at its size
static void place(struct flame_lock *lock) {
	struct wlr_box all;
	wlr_output_layout_get_box(lock->server->outputs.layout, NULL, &all);
	scene_node_set_position(&lock->cover->node, all.x, all.y);
	scene_rect_set_size(lock->cover, all.width, all.height);
	struct lock_surface *surface;
	wl_list_for_each(surface, &lock->surfaces, link) {
		struct flame_output *output = surface->wlr->output->data;
		if (!output) {
			continue;
		}
		scene_node_set_position(&surface->tree->node, output->box.x, output->box.y);
		wlr_session_lock_surface_v1_configure(surface->wlr, (uint32_t)output->box.width,
			(uint32_t)output->box.height);
	}
}

static void handle_layout_change(struct wl_listener *listener, void *data) {
	struct flame_lock *lock = wl_container_of(listener, lock, layout_change);
	if (lock->locked) {
		place(lock);
	}
}

// ---- the keyboard ----

static struct lock_surface *first_mapped(struct flame_lock *lock) {
	struct lock_surface *surface;
	wl_list_for_each(surface, &lock->surfaces, link) {
		if (surface->wlr->surface->mapped) {
			return surface;
		}
	}
	return NULL;
}

// the keyboard to one of the locker's surfaces, or to nobody
static void focus_locker(struct flame_lock *lock, struct lock_surface *surface) {
	struct flame_input *input = &lock->server->input;
	input_keyboard_enter(input, surface ? surface->wlr->surface : NULL);
}

static bool has_keyboard(struct flame_lock *lock, struct lock_surface *surface) {
	return lock->server->input.seat->keyboard_state.focused_surface == surface->wlr->surface;
}

// ---- the locker's surfaces ----

static void handle_surface_map(struct wl_listener *listener, void *data) {
	struct lock_surface *surface = wl_container_of(listener, surface, map);
	struct flame_lock *lock = surface->lock;
	struct wlr_surface *focused = lock->server->input.seat->keyboard_state.focused_surface;
	// the first one up takes the keys
	if (!focused || !wlr_session_lock_surface_v1_try_from_wlr_surface(focused)) {
		focus_locker(lock, surface);
	}
}

static void handle_surface_destroy(struct wl_listener *listener, void *data) {
	struct lock_surface *surface = wl_container_of(listener, surface, destroy);
	struct flame_lock *lock = surface->lock;
	bool had_keyboard = has_keyboard(lock, surface);
	listener_disconnect(&surface->map);
	listener_disconnect(&surface->destroy);
	wl_list_remove(&surface->link);
	scene_node_destroy(&surface->tree->node);
	free(surface);
	if (had_keyboard && lock->locked) {
		focus_locker(lock, first_mapped(lock));
	}
}

static void handle_new_surface(struct wl_listener *listener, void *data) {
	struct flame_lock *lock = wl_container_of(listener, lock, new_surface);
	struct wlr_session_lock_surface_v1 *wlr = data;
	struct lock_surface *surface = calloc(1, sizeof(*surface));
	if (!surface) {
		wlr_log(WLR_ERROR, "lock: out of memory");
		return; // that screen shows the cover
	}
	surface->lock = lock;
	surface->wlr = wlr;
	surface->tree = scene_tree_create(lock->tree);
	if (!surface->tree || !scene_surface_create(surface->tree, wlr->surface)) {
		wlr_log(WLR_ERROR, "lock: no scene for a lock surface");
		if (surface->tree) {
			scene_node_destroy(&surface->tree->node);
		}
		free(surface);
		return;
	}
	wl_list_insert(&lock->surfaces, &surface->link);
	listener_connect(&surface->map, &wlr->surface->events.map, handle_surface_map);
	listener_connect(&surface->destroy, &wlr->events.destroy, handle_surface_destroy);
	place(lock);
}

// ---- locking, unlocking ----

static void forget_locker(struct flame_lock *lock) {
	listener_disconnect(&lock->unlock);
	listener_disconnect(&lock->lock_destroy);
	listener_disconnect(&lock->new_surface);
	lock->lock = NULL;
	waits_clear(lock);
	scene_node_set_enabled(&lock->backdrop->node, false);
}

static void handle_unlock(struct wl_listener *listener, void *data) {
	struct flame_lock *lock = wl_container_of(listener, lock, unlock);
	struct flame_server *server = lock->server;
	wlr_log(WLR_INFO, "lock: unlocked");
	forget_locker(lock);
	lock->locked = false;
	scene_node_set_enabled(&lock->tree->node, false);
	// back to where things were: the window that had the keys, a launcher,
	// whatever's under the pointer
	input_keyboard_enter(&server->input, NULL);
	focus_restore_keyboard(&server->desktop);
	layers_update_focus(&server->layers);
	cursor_refresh(&server->input.cursor);
}

static void handle_lock_destroy(struct wl_listener *listener, void *data) {
	struct flame_lock *lock = wl_container_of(listener, lock, lock_destroy);
	// gone without unlocking (it crashed, it was killed): still locked, the
	// cover stays. a new locker can take over
	wlr_log(WLR_ERROR, "lock: the locker went away without unlocking, staying locked");
	forget_locker(lock);
}

static void handle_new_lock(struct wl_listener *listener, void *data) {
	struct flame_lock *lock = wl_container_of(listener, lock, new_lock);
	struct wlr_session_lock_v1 *wlr = data;
	struct flame_server *server = lock->server;
	if (lock->lock) {
		// one's locked it already and it's still there: not another
		wlr_log(WLR_INFO, "lock: already locked, a second locker turned away");
		wlr_session_lock_v1_destroy(wlr);
		return;
	}
	if (lock->locked) {
		wlr_log(WLR_INFO, "lock: a new locker takes over");
	} else {
		wlr_log(WLR_INFO, "lock: locking");
	}
	lock->lock = wlr;
	lock->told = false;
	listener_connect(&lock->unlock, &wlr->events.unlock, handle_unlock);
	listener_connect(&lock->lock_destroy, &wlr->events.destroy, handle_lock_destroy);
	listener_connect(&lock->new_surface, &wlr->events.new_surface, handle_new_surface);
	scene_node_set_enabled(&lock->backdrop->node, true);
	if (!lock->locked) {
		lock->locked = true;
		scene_node_set_enabled(&lock->tree->node, true);
		place(lock);
		// nothing that had the keys or the pointer keeps them
		input_keyboard_enter(&server->input, NULL);
		wlr_seat_pointer_notify_clear_focus(server->input.seat);
	}
	wait_for_screens(lock);
}

// ---- the pointer ----

void lock_pointer(struct flame_lock *lock, double lx, double ly, uint32_t time_msec,
		bool pressed) {
	struct wlr_seat *seat = lock->server->input.seat;
	double sx, sy;
	struct wlr_surface *surface = NULL;
	scene_node_at(&lock->server->render.scene, lx, ly, &surface, &sx, &sy);
	if (!surface || !lock_allows(lock, surface)) {
		wlr_seat_pointer_notify_clear_focus(seat);
		return;
	}
	wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
	wlr_seat_pointer_notify_motion(seat, time_msec, sx, sy);
	if (pressed && seat->keyboard_state.focused_surface != surface) {
		// another screen's locker clicked: the keys go there
		input_keyboard_enter(&lock->server->input, surface);
	}
}

// ---- setting up ----

bool lock_init(struct flame_lock *lock, struct flame_server *server) {
	lock->server = server;
	wl_list_init(&lock->surfaces);
	wl_list_init(&lock->waits);
	lock->manager = wlr_session_lock_manager_v1_create(server->display);
	lock->tree = scene_tree_create(server->render.layers[FLAME_LAYER_LOCK]);
	lock->cover = lock->tree ? scene_rect_create(lock->tree, 0, 0, cover_color) : NULL;
	lock->backdrop = lock->cover ? scene_tree_create(lock->tree) : NULL;
	if (!lock->manager || !lock->backdrop) {
		wlr_log(WLR_ERROR, "lock: failed to set up");
		return false;
	}
	scene_node_set_enabled(&lock->tree->node, false);
	listener_connect(&lock->new_lock, &lock->manager->events.new_lock, handle_new_lock);
	listener_connect(&lock->layout_change, &server->outputs.layout->events.change,
		handle_layout_change);
	return sleep_init(&lock->sleep, server);
}

void lock_finish(struct flame_lock *lock) {
	sleep_finish(&lock->sleep);
	if (lock->lock) {
		forget_locker(lock);
	}
	listener_disconnect(&lock->new_lock);
	listener_disconnect(&lock->layout_change);
}
