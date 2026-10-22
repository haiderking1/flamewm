#include "transaction/transaction.h"

#include <math.h>
#include <stdlib.h>
#include <pixman.h>
#include <time.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/util/log.h>

#include "core/server.h"
#include "util/listener.h"
#include "util/macros.h"

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

// applying from idle keeps us out of whatever handler noticed readiness
static void handle_apply_idle(void *data);
static void schedule_apply(struct flame_transaction *txn) {
	if (!txn->apply_idle) {
		txn->apply_idle = wl_event_loop_add_idle(txn->server->loop,
			handle_apply_idle, txn);
	}
}

// a desync subsurface of a window in flight
struct txn_child {
	struct wl_list link; // flame_view_txn.children
	struct flame_view *view;
	struct wlr_surface *surface;
	// it was filling the old size: the window isn't ready until this one
	// attached a buffer at a new size, or there'd be a strip of old content
	bool stale;
	bool was_opaque;
	int unfinished; // new size buffers so far that looked half drawn
	int old_buffer_width, old_buffer_height;
	int old_width, old_height;
	bool locked;
	uint32_t lock_seq;
	struct wl_listener client_commit;
	struct wl_listener destroy;
};

// a held commit's frame callbacks would only fire once we release it, and
// clients that wait on them (firefox's content) would never draw what we're
// waiting for
static void release_pending_callbacks(struct wlr_surface *surface) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	uint32_t msec = (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
	struct wl_resource *resource, *tmp;
	wl_resource_for_each_safe(resource, tmp, &surface->pending.frame_callback_list) {
		wl_callback_send_done(resource, msec);
		wl_resource_destroy(resource);
	}
}

// ready = the root acked our configure and every full-window desync child
// caught up. both are held, so they get released in the same frame
static void refresh_ready(struct flame_view *view) {
	if (view->txn.ready || !view->txn.locked || view->txn.root_stale) {
		return;
	}
	struct txn_child *child;
	wl_list_for_each(child, &view->txn.children, link) {
		if (child->stale) {
			return;
		}
	}
	view->txn.ready = true;
	struct flame_transaction *txn = &view->desktop->transaction;
	if (all_ready(txn)) {
		schedule_apply(txn);
	}
}

// opaque over all of area (surface coords). an empty area means the whole
// surface. chromium keeps a transparent margin around its window, only the
// window itself has to be covered
static bool fully_opaque(const struct wlr_surface_state *state, struct wlr_box area) {
	struct wlr_box surface = {0, 0, state->width, state->height};
	struct wlr_box inside;
	area = !wlr_box_empty(&area) && wlr_box_intersection(&inside, &surface, &area)
		? inside : surface;
	pixman_box32_t box = {area.x, area.y, area.x + area.width, area.y + area.height};
	return !wlr_box_empty(&area)
		&& pixman_region32_contains_rectangle((pixman_region32_t *)&state->opaque, &box)
			== PIXMAN_REGION_IN;
}

static struct wlr_box root_area(struct flame_view *view, bool pending) {
	return view->impl->window_geometry(view, pending);
}

static const struct wlr_box whole_surface = {0};

// past this many new size buffers that still look half drawn, take it
// anyway: a few frames late beats stalling until the deadline
#define MAX_UNFINISHED_FRAMES 3

// the buffer shows up pixel for pixel: no viewport stretching it, at most
// cropping it. chromium keeps one oversized buffer and crops it to whatever
// size it drew this time
static bool unscaled(const struct wlr_surface_state *state) {
	double width, height;
	if (state->viewport.has_src) {
		width = state->viewport.src.width;
		height = state->viewport.src.height;
	} else {
		int scale = state->scale > 0 ? state->scale : 1;
		width = (double)state->buffer_width / scale;
		height = (double)state->buffer_height / scale;
		if (state->transform & WL_OUTPUT_TRANSFORM_90) {
			double tmp = width;
			width = height;
			height = tmp;
		}
	}
	return fabs(width - state->width) < 0.5 && fabs(height - state->height) < 0.5;
}

struct old_size {
	int buffer_width, buffer_height;
	int width, height;
};

// the commit being made attaches a buffer drawn at a new size: the buffer
// itself changed size, or the surface did while showing it unscaled. a
// viewport stretching the old buffer to the new size (firefox does that
// before it draws) doesn't count
static bool new_size_buffer(struct wlr_surface *surface, struct old_size old) {
	const struct wlr_surface_state *state = &surface->pending;
	if (!(state->committed & WLR_SURFACE_STATE_BUFFER) || !state->buffer) {
		return false;
	}
	if (state->buffer_width != old.buffer_width || state->buffer_height != old.buffer_height) {
		return true;
	}
	return (state->width != old.width || state->height != old.height) && unscaled(state);
}

// firefox's first frame at a new size has the old layout painted into it, the
// rest filled black, and only declares the old area as opaque. that half
// painted frame is the right/bottom strip. so a surface that used to be fully
// opaque and now declares only part of itself opaque isn't done yet. an empty
// opaque region just means it stopped claiming opacity, that one's fine
static bool looks_finished(struct wlr_surface *surface, struct wlr_box area,
		bool was_opaque) {
	return !was_opaque || fully_opaque(&surface->pending, area)
		|| !pixman_region32_not_empty(&surface->pending.opaque);
}

// a new size buffer that looks finished, or enough of them that we stop
// waiting for one that does
static bool redraws(struct wlr_surface *surface, struct old_size old, struct wlr_box area,
		bool was_opaque, int *unfinished) {
	if (!new_size_buffer(surface, old)) {
		return false;
	}
	if (looks_finished(surface, area, was_opaque)) {
		return true;
	}
	return ++*unfinished >= MAX_UNFINISHED_FRAMES;
}

// what a window in flight just committed, for chasing client quirks in
// /tmp/flamewm.log
static void log_commit(const char *who, struct flame_view *view, struct wlr_surface *surface) {
	pixman_box32_t *opaque = pixman_region32_extents(&surface->pending.opaque);
	wlr_log(WLR_DEBUG, "txn %p %s commit: size %dx%d buffer %dx%d new=%d opaque %d,%d %dx%d "
		"full_opaque=%d locked=%d stale=%d", (void *)view, who,
		surface->pending.width, surface->pending.height,
		surface->pending.buffer_width, surface->pending.buffer_height,
		(surface->pending.committed & WLR_SURFACE_STATE_BUFFER) && surface->pending.buffer,
		opaque->x1, opaque->y1, opaque->x2 - opaque->x1, opaque->y2 - opaque->y1,
		fully_opaque(&surface->pending, whole_surface), view->txn.locked, view->txn.root_stale);
}

static void child_free(struct txn_child *child) {
	listener_disconnect(&child->client_commit);
	listener_disconnect(&child->destroy);
	wl_list_remove(&child->link);
	free(child);
}

static void handle_child_commit(struct wl_listener *listener, void *data) {
	struct txn_child *child = wl_container_of(listener, child, client_commit);
	struct wlr_surface *surface = child->surface;
	struct flame_view *view = child->view;

	log_commit("child", view, surface);
	int old_width, old_height;
	view_content_size(view, view->geometry, view->fullscreen_shown, &old_width, &old_height);
	bool resized = surface->pending.width != old_width || surface->pending.height != old_height;
	// firefox resizes through its viewport first and only draws the new size
	// a few commits later, so a new size alone doesn't count, the pixels do
	struct old_size old = {child->old_buffer_width, child->old_buffer_height,
		child->old_width, child->old_height};
	bool redrawn = redraws(surface, old, whole_surface, child->was_opaque, &child->unfinished);

	if (!child->locked) {
		// old size frames still fit the old box, let them through
		if (!resized && !redrawn) {
			return;
		}
		// it moved on: hold it here, the rest queues up behind
		child->lock_seq = wlr_surface_lock_pending(surface);
		child->locked = true;
	}
	// held commits would sit on their frame callbacks, keep it drawing
	release_pending_callbacks(surface);
	if (child->stale && redrawn) {
		child->stale = false;
		refresh_ready(view);
	}
}

static void handle_child_destroy(struct wl_listener *listener, void *data) {
	// its cached state dies with it, nothing to release
	struct txn_child *child = wl_container_of(listener, child, destroy);
	struct flame_view *view = child->view;
	child_free(child);
	refresh_ready(view);
}

static bool subsurface_synced(struct wlr_subsurface *subsurface) {
	while (subsurface) {
		if (subsurface->synchronized) {
			return true;
		}
		subsurface = wlr_subsurface_try_from_wlr_surface(subsurface->parent);
	}
	return false;
}

struct track_context {
	struct flame_view *view;
	int old_width, old_height;
	bool resizing;
};

static void track_child(struct wlr_surface *surface, int sx, int sy, void *data) {
	struct track_context *context = data;
	struct wlr_subsurface *subsurface = wlr_subsurface_try_from_wlr_surface(surface);
	// synced ones are cached with the root and held by its lock already
	if (!subsurface || subsurface_synced(subsurface) || !surface->mapped) {
		return;
	}
	struct txn_child *child = calloc(1, sizeof(*child));
	if (!child) {
		wlr_log(WLR_ERROR, "out of memory");
		return;
	}
	child->view = context->view;
	child->surface = surface;
	// only full-window ones matter, the rest can't leave a strip
	if (!context->resizing || surface->current.width != context->old_width
			|| surface->current.height != context->old_height) {
		free(child);
		return;
	}
	child->stale = true;
	child->was_opaque = fully_opaque(&surface->current, whole_surface);
	child->old_buffer_width = surface->current.buffer_width;
	child->old_buffer_height = surface->current.buffer_height;
	child->old_width = surface->current.width;
	child->old_height = surface->current.height;
	listener_connect(&child->client_commit, &surface->events.client_commit,
		handle_child_commit);
	listener_connect(&child->destroy, &surface->events.destroy, handle_child_destroy);
	wl_list_insert(&context->view->txn.children, &child->link);
}

static void track_children(struct flame_view *view) {
	struct track_context context = {.view = view};
	int width, height;
	view_content_size(view, view->txn.target, view->txn.target_fullscreen, &width, &height);
	if (view->shown) {
		view_content_size(view, view->geometry, view->fullscreen_shown,
			&context.old_width, &context.old_height);
		context.resizing = width != context.old_width || height != context.old_height;
	}

	// same rule for the root as for desync children: if it was filling the
	// old size, its new size has to be drawn before the window shows up
	struct wlr_surface *root = view_surface(view);
	view->txn.root_stale = context.resizing && root->current.width == context.old_width
		&& root->current.height == context.old_height;
	view->txn.root_was_opaque = fully_opaque(&root->current, root_area(view, false));
	view->txn.root_unfinished = 0;
	view->txn.old_buffer_width = root->current.buffer_width;
	view->txn.old_buffer_height = root->current.buffer_height;
	view->txn.old_width = root->current.width;
	view->txn.old_height = root->current.height;

	wlr_surface_for_each_surface(root, track_child, &context);

	int children = 0;
	struct txn_child *child;
	wl_list_for_each(child, &view->txn.children, link) {
		children++;
	}
	wlr_log(WLR_DEBUG, "txn %p tracking: content %dx%d -> %dx%d root %dx%d buffer %dx%d "
		"opaque=%d stale=%d, %d full window desync children", (void *)view,
		context.old_width, context.old_height, width, height,
		root->current.width, root->current.height, root->current.buffer_width,
		root->current.buffer_height, view->txn.root_was_opaque, view->txn.root_stale, children);
}

struct held {
	struct wlr_surface *surface;
	uint32_t seq;
};

static int count_locks(struct flame_view *view) {
	int count = view->txn.locked;
	struct txn_child *child;
	wl_list_for_each(child, &view->txn.children, link) {
		count += child->locked;
	}
	return count;
}

// drops all of the view's bookkeeping and hands its locks to out (up to
// cap, out may be NULL with cap 0). nothing gets released here
static int detach(struct flame_view *view, struct held *out, int cap) {
	int count = 0;
	if (view->txn.locked) {
		if (count < cap) {
			out[count] = (struct held){view_surface(view), view->txn.lock_seq};
		}
		count++;
	}
	struct txn_child *child, *tmp;
	wl_list_for_each_safe(child, tmp, &view->txn.children, link) {
		if (child->locked) {
			if (count < cap) {
				out[count] = (struct held){child->surface, child->lock_seq};
			}
			count++;
		}
		child_free(child);
	}
	if (count > cap) {
		wlr_log(WLR_ERROR, "out of memory, %d surface locks leaked", count - cap);
	}

	listener_disconnect(&view->txn.client_commit);
	wl_list_remove(&view->txn.link);
	wl_list_init(&view->txn.link);
	view->txn.ready = false;
	view->txn.locked = false;
	view->txn.root_stale = false;
	return count;
}

// unmap can come from inside surface destruction, so these wait an idle
static void detach_and_release_later(struct flame_transaction *txn, struct flame_view *view) {
	int count = count_locks(view);
	struct held *held = count ? calloc(count, sizeof(*held)) : NULL;
	int cap = held ? count : 0;
	// detach() has side effects, so never inside MIN(), it'd run twice
	int detached = detach(view, held, cap);
	int collected = MIN(detached, cap);
	for (int i = 0; i < collected; i++) {
		unlock_later(txn->server->loop, held[i].surface, held[i].seq);
	}
	free(held);
}

static void apply(struct flame_transaction *txn) {
	if (txn->apply_idle) {
		wl_event_source_remove(txn->apply_idle);
		txn->apply_idle = NULL;
	}
	wl_event_source_timer_update(txn->deadline, 0);

	// place everything first so released commits land in their final boxes
	int count = 0, late = 0, held = 0;
	struct flame_view *view;
	wl_list_for_each(view, &txn->participants, txn.link) {
		wlr_log(WLR_DEBUG, "txn %p placed at %d,%d %dx%d fullscreen=%d ready=%d", (void *)view,
			view->txn.target.x, view->txn.target.y, view->txn.target.width,
			view->txn.target.height, view->txn.target_fullscreen, view->txn.ready);
		view_place(view, view->txn.target, view->txn.target_fullscreen);
		count++;
		late += !view->txn.ready;
		held += count_locks(view);
	}
	wlr_log(WLR_DEBUG, "transaction applied: %d views, %d late", count, late);

	// then detach everyone before releasing anything: a released commit can
	// unmap its view, and that must not find a half applied transaction
	struct held *locks = calloc(held ? held : 1, sizeof(*locks));
	int collected = 0;
	struct flame_view *tmp;
	wl_list_for_each_safe(view, tmp, &txn->participants, txn.link) {
		if (locks) {
			int room = held - collected;
			int detached = detach(view, locks + collected, room);
			collected += MIN(detached, room);
		} else {
			detach_and_release_later(txn, view);
		}
	}
	txn->active = false;

	// roots first so their subsurface positions are in place for the children
	for (int i = 0; i < collected; i++) {
		wlr_surface_unlock_cached(locks[i].surface, locks[i].seq);
	}
	free(locks);

	desktop_layout_applied(&txn->server->desktop);

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

static int handle_deadline(void *data) {
	struct flame_transaction *txn = data;
	struct flame_view *view;
	wl_list_for_each(view, &txn->participants, txn.link) {
		if (view->txn.ready) {
			continue;
		}
		int stale = 0;
		struct txn_child *child;
		wl_list_for_each(child, &view->txn.children, link) {
			stale += child->stale;
		}
		wlr_log(WLR_DEBUG, "late window: acked=%d undrawn=%d stale children=%d shown=%d",
			view->txn.locked, view->txn.root_stale, stale, view->shown);
	}
	wlr_log(WLR_DEBUG, "transaction deadline hit, applying anyway");
	apply(txn);
	return 0;
}

static void handle_client_commit(struct wl_listener *listener, void *data) {
	struct flame_view *view = wl_container_of(listener, view, txn.client_commit);

	struct wlr_surface *surface = view_surface(view);
	log_commit("root", view, surface);
	struct old_size old = {view->txn.old_buffer_width, view->txn.old_buffer_height,
		view->txn.old_width, view->txn.old_height};
	bool redrawn = redraws(surface, old, root_area(view, true), view->txn.root_was_opaque,
		&view->txn.root_unfinished);
	if (view->txn.locked) {
		// queued behind the one we hold, keep it drawing
		release_pending_callbacks(surface);
		if (view->txn.root_stale && redrawn) {
			view->txn.root_stale = false;
			refresh_ready(view);
		}
		return;
	}
	if (!view->impl->acked(view, view->txn.serial)) {
		// older frames still show up in the old box, that's fine
		return;
	}

	// it acked: hold this one, everything after queues up behind it
	view->txn.locked = true;
	view->txn.lock_seq = wlr_surface_lock_pending(surface);
	release_pending_callbacks(surface);
	if (redrawn) {
		view->txn.root_stale = false;
	}
	refresh_ready(view);
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
	wl_list_init(&view->txn.children);
}

bool transaction_active(struct flame_transaction *txn) {
	return txn->active;
}

bool transaction_has(struct flame_view *view) {
	return !wl_list_empty(&view->txn.link);
}

void transaction_add(struct flame_transaction *txn, struct flame_view *view,
		struct wlr_box target, bool fullscreen) {
	if (txn->active || transaction_has(view)) {
		return;
	}

	uint32_t serial = view_configure(view, target, fullscreen);
	if (serial == 0 && view->shown && wlr_box_equal(&target, &view->geometry)
			&& fullscreen == view->fullscreen_shown) {
		return; // nothing about this one changes
	}

	view->txn.target = target;
	view->txn.target_fullscreen = fullscreen;
	view->txn.serial = serial;
	view->txn.ready = serial == 0; // position only, or already the right size
	view->txn.locked = false;
	wl_list_insert(txn->participants.prev, &view->txn.link);

	if (!view->txn.ready) {
		listener_connect(&view->txn.client_commit,
			&view_surface(view)->events.client_commit, handle_client_commit);
		track_children(view);
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
	detach_and_release_later(txn, view);

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
		if (!view->txn.ready) {
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
