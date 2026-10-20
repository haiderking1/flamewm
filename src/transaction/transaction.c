#include "transaction/transaction.h"

#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/util/log.h>

#include "core/server.h"
#include "util/listener.h"

// a client that never answers only gets to hold everyone up this long
#define DEADLINE_MS 300

// a held commit whose view went away. released on the next idle, unless
// the surface dies first and takes the cached state with it
struct deferred_unlock {
	struct wlr_surface *surface;
	uint32_t seq;
	struct wl_event_source *idle;
	struct wl_listener destroy;
};

static void deferred_unlock_free(struct deferred_unlock *unlock) {
	listener_disconnect(&unlock->destroy);
	free(unlock);
}

static void handle_deferred_idle(void *data) {
	struct deferred_unlock *unlock = data;
	wlr_surface_unlock_cached(unlock->surface, unlock->seq);
	deferred_unlock_free(unlock);
}

static void handle_deferred_destroy(struct wl_listener *listener, void *data) {
	struct deferred_unlock *unlock = wl_container_of(listener, unlock, destroy);
	wl_event_source_remove(unlock->idle);
	deferred_unlock_free(unlock);
}

// unmap can fire from inside surface destruction, unlocking right there
// would apply state on a half dead surface
static void unlock_later(struct wl_event_loop *loop, struct wlr_surface *surface,
		uint32_t seq) {
	struct deferred_unlock *unlock = calloc(1, sizeof(*unlock));
	if (!unlock) {
		// leaking a lock beats applying state mid destruction
		wlr_log(WLR_ERROR, "out of memory, dropping a surface lock");
		return;
	}
	unlock->surface = surface;
	unlock->seq = seq;
	unlock->idle = wl_event_loop_add_idle(loop, handle_deferred_idle, unlock);
	listener_connect(&unlock->destroy, &surface->events.destroy, handle_deferred_destroy);
}

static bool all_ready(struct flame_transaction *txn) {
	struct flame_view *view;
	wl_list_for_each(view, &txn->participants, txn.link) {
		if (!view->txn.ready) {
			return false;
		}
	}
	return true;
}

static void detach(struct flame_view *view) {
	listener_disconnect(&view->txn.client_commit);
	wl_list_remove(&view->txn.link);
	wl_list_init(&view->txn.link);
	view->txn.ready = false;
	view->txn.locked = false;
}

static void apply(struct flame_transaction *txn) {
	if (txn->apply_idle) {
		wl_event_source_remove(txn->apply_idle);
		txn->apply_idle = NULL;
	}
	wl_event_source_timer_update(txn->deadline, 0);

	// place everything first so released commits land in their final boxes
	int count = 0, late = 0;
	struct flame_view *view;
	wl_list_for_each(view, &txn->participants, txn.link) {
		view_place(view, view->txn.target);
		count++;
		late += !view->txn.ready;
	}
	wlr_log(WLR_DEBUG, "transaction applied: %d views, %d late", count, late);

	// then detach everyone before releasing anything: a released commit can
	// unmap its view, and that must not find a half applied transaction
	struct { struct wlr_surface *surface; uint32_t seq; } *locks =
		calloc(count ? count : 1, sizeof(*locks));
	int locked = 0;
	struct flame_view *tmp;
	wl_list_for_each_safe(view, tmp, &txn->participants, txn.link) {
		if (view->txn.locked && locks) {
			locks[locked].surface = view_surface(view);
			locks[locked].seq = view->txn.lock_seq;
			locked++;
		} else if (view->txn.locked) {
			unlock_later(txn->server->loop, view_surface(view), view->txn.lock_seq);
		}
		detach(view);
	}
	txn->active = false;

	for (int i = 0; i < locked; i++) {
		wlr_surface_unlock_cached(locks[i].surface, locks[i].seq);
	}
	free(locks);

	cursor_refresh(&txn->server->input.cursor);

	if (txn->rearrange) {
		txn->rearrange = false;
		desktop_arrange(&txn->server->desktop);
	}
}

static void handle_apply_idle(void *data) {
	struct flame_transaction *txn = data;
	txn->apply_idle = NULL;
	apply(txn);
}

// applying from idle keeps us out of whatever handler noticed readiness
static void schedule_apply(struct flame_transaction *txn) {
	if (!txn->apply_idle) {
		txn->apply_idle = wl_event_loop_add_idle(txn->server->loop,
			handle_apply_idle, txn);
	}
}

static int handle_deadline(void *data) {
	struct flame_transaction *txn = data;
	wlr_log(WLR_DEBUG, "transaction deadline hit, applying anyway");
	apply(txn);
	return 0;
}

static void handle_client_commit(struct wl_listener *listener, void *data) {
	struct flame_view *view = wl_container_of(listener, view, txn.client_commit);
	struct flame_transaction *txn = &view->desktop->transaction;

	if (view->txn.ready || !view->impl->acked(view, view->txn.serial)) {
		// older frames still show up in the old box, that's fine
		return;
	}

	// this commit carries the new size, hold it until everyone is there
	view->txn.ready = true;
	view->txn.locked = true;
	view->txn.lock_seq = wlr_surface_lock_pending(view_surface(view));
	listener_disconnect(&view->txn.client_commit);

	if (all_ready(txn)) {
		schedule_apply(txn);
	}
}

bool transaction_init(struct flame_transaction *txn, struct flame_server *server) {
	txn->server = server;
	wl_list_init(&txn->participants);
	txn->deadline = wl_event_loop_add_timer(server->loop, handle_deadline, txn);
	if (!txn->deadline) {
		wlr_log(WLR_ERROR, "failed to create transaction timer");
		return false;
	}
	return true;
}

void transaction_finish(struct flame_transaction *txn) {
	if (txn->apply_idle) {
		wl_event_source_remove(txn->apply_idle);
		txn->apply_idle = NULL;
	}
	wl_event_source_remove(txn->deadline);
}

void transaction_view_init(struct flame_view *view) {
	wl_list_init(&view->txn.link);
	wl_list_init(&view->txn.client_commit.link);
}

bool transaction_active(struct flame_transaction *txn) {
	return txn->active;
}

bool transaction_has(struct flame_view *view) {
	return !wl_list_empty(&view->txn.link);
}

void transaction_add(struct flame_transaction *txn, struct flame_view *view,
		struct wlr_box target) {
	if (txn->active || transaction_has(view)) {
		return;
	}

	uint32_t serial = view_configure(view, target);
	if (serial == 0 && view->shown && wlr_box_equal(&target, &view->geometry)) {
		return; // nothing about this one changes
	}

	view->txn.target = target;
	view->txn.serial = serial;
	view->txn.ready = serial == 0; // position only, or already the right size
	view->txn.locked = false;
	wl_list_insert(txn->participants.prev, &view->txn.link);

	if (!view->txn.ready) {
		listener_connect(&view->txn.client_commit,
			&view_surface(view)->events.client_commit, handle_client_commit);
	}
}

void transaction_commit(struct flame_transaction *txn) {
	if (txn->active || wl_list_empty(&txn->participants)) {
		return;
	}
	txn->active = true;
	if (all_ready(txn)) {
		schedule_apply(txn);
	} else {
		wl_event_source_timer_update(txn->deadline, DEADLINE_MS);
		// start the frame heartbeat for hidden windows
		outputs_schedule_frame(&txn->server->outputs);
	}
}

void transaction_remove(struct flame_transaction *txn, struct flame_view *view) {
	if (!transaction_has(view)) {
		return;
	}
	if (view->txn.locked) {
		unlock_later(txn->server->loop, view_surface(view), view->txn.lock_seq);
	}
	detach(view);

	// even with nobody left, apply still runs a rearrange that may be owed
	if (txn->active && all_ready(txn)) {
		schedule_apply(txn);
	}
}

static void send_frame_done(struct wlr_surface *surface, int sx, int sy, void *data) {
	wlr_surface_send_frame_done(surface, data);
}

bool transaction_frame_done(struct flame_transaction *txn, const struct timespec *now) {
	bool waiting = false;
	struct flame_view *view;
	wl_list_for_each(view, &txn->participants, txn.link) {
		if (!view->shown && !view->txn.ready) {
			wlr_surface_for_each_surface(view_surface(view), send_frame_done, (void *)now);
			waiting = true;
		}
	}
	return waiting;
}

void transaction_flush(struct flame_transaction *txn) {
	if (txn->active) {
		apply(txn);
	}
}
