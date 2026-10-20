#ifndef FLAME_TRANSACTION_TRANSACTION_H
#define FLAME_TRANSACTION_TRANSACTION_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/util/box.h>

struct flame_server;
struct flame_view;

// a layout change lands in one frame or not at all. every window keeps
// showing its old frame, the commit where a client takes its new size gets
// held, and once everyone is ready (or the deadline hits) all of it is
// placed and released together. same idea as raven's resize batches

// per view bookkeeping, lives inside struct flame_view
struct flame_view_txn {
	struct wl_list link; // flame_transaction.participants, empty when not in one
	struct wlr_box target;
	bool target_fullscreen;
	uint32_t serial; // configure we're waiting on
	bool ready;
	bool locked; // holding the root commit that acked serial
	uint32_t lock_seq;
	// an ack only says the configure arrived, not that it got drawn. a root
	// that filled the old size waits for a buffer at a new size too
	bool root_stale;
	bool root_was_opaque;
	int root_unfinished; // new size buffers so far that looked half drawn
	int old_buffer_width, old_buffer_height;
	int old_width, old_height; // surface size before, viewport included
	struct wl_listener client_commit;
	// desync subsurfaces (firefox's web content) commit on their own clock,
	// so they get held too or they'd resize before their window does
	struct wl_list children; // txn_child.link
};

struct flame_transaction {
	struct flame_server *server;
	struct wl_list participants; // flame_view.txn.link
	struct wl_event_source *deadline;
	struct wl_event_source *apply_idle;
	bool active;
	bool rearrange; // layout asked for again mid flight, redo after apply
};

bool transaction_init(struct flame_transaction *txn, struct flame_server *server);
void transaction_finish(struct flame_transaction *txn);

void transaction_view_init(struct flame_view *view);

bool transaction_active(struct flame_transaction *txn);
bool transaction_has(struct flame_view *view);

// collect targets for a fresh transaction, then commit it
void transaction_add(struct flame_transaction *txn, struct flame_view *view,
	struct wlr_box target, bool fullscreen);
void transaction_commit(struct flame_transaction *txn);

// view unmapped mid flight, stop waiting on it
void transaction_remove(struct flame_transaction *txn, struct flame_view *view);
// hidden or covered windows (new ones, tiles behind a fullscreen) get no frame
// callbacks from the scene, and clients like foot wait for one before drawing
// their new size. call on every output frame, true means someone's still
// waiting so keep the frames coming
bool transaction_frame_done(struct flame_transaction *txn, const struct timespec *now);
// apply right now, ready or not
void transaction_flush(struct flame_transaction *txn);

#endif
