#include <stdlib.h>
#include <string.h>
#include <wlr/util/log.h>

#include "xserver/internal.h"

// selections: who owns which, and handing conversions between requestor and
// owner. flame takes part too, for the clipboard and drag and drop between
// X and wayland apps: it can own a selection (requests for it come to flame
// through hooks.selection_request) and ask an X owner for a selection's
// contents (xserver_selection_convert), with a window of its own per ask,
// big answers coming in pieces (INCR) included. XFIXES' selection events
// tell clients when owners change

#define TRANSFER_TIMEOUT_MS 10000
#define FLAME_WINDOWS 0x10000 // flame's own windows, below every client's ids

// XFIXES selection events
#define SET_SELECTION_OWNER 0
#define SELECTION_WINDOW_DESTROY 1
#define SELECTION_CLIENT_CLOSE 2

struct xs_transfer {
	struct wl_list link; // flame_xserver.transfers
	struct flame_xserver *xserver;
	struct xs_window *window; // flame's, where the answer goes
	uint32_t selection, target, property;
	bool incr; // coming in pieces
	struct wl_event_source *timeout;
	xs_transfer_fn fn;
	void *data;
};

// a client wants to hear when a selection's owner changes (XFIXES)
struct select {
	struct wl_list link; // flame_xserver.selection_selects
	struct xs_client *client;
	uint32_t window, selection, mask;
};

static struct xs_transfer *transfer_of(struct flame_xserver *xserver,
		struct xs_window *window) {
	struct xs_transfer *transfer;
	wl_list_for_each(transfer, &xserver->transfers, link) {
		if (transfer->window == window) {
			return transfer;
		}
	}
	return NULL;
}

static uint32_t atom(struct flame_xserver *xserver, const char *name) {
	return xs_atom_intern(&xserver->atoms, name, strlen(name), true);
}

static struct xs_selection *find_selection(struct flame_xserver *xserver, uint32_t atom,
		bool create) {
	struct xs_selection *selection;
	wl_list_for_each(selection, &xserver->selections, link) {
		if (selection->atom == atom) {
			return selection;
		}
	}
	if (!create || !(selection = calloc(1, sizeof(*selection)))) {
		return NULL;
	}
	selection->atom = atom;
	wl_list_insert(&xserver->selections, &selection->link);
	return selection;
}

// XFIXES SelectionNotify to everyone listening for selection
static void owner_event(struct flame_xserver *xserver, struct xs_selection *selection,
		uint8_t subtype) {
	struct select *select;
	wl_list_for_each(select, &xserver->selection_selects, link) {
		if (select->selection != selection->atom || !(select->mask & (1u << subtype))) {
			continue;
		}
		struct xs_event event = {0};
		xs_event_put8(&event, 0, XS_XFIXES_FIRST_EVENT);
		xs_event_put8(&event, 1, subtype);
		xs_event_put32(&event, 4, select->window);
		xs_event_put32(&event, 8, selection->owner);
		xs_event_put32(&event, 12, selection->atom);
		xs_event_put32(&event, 16, xs_now(xserver));
		xs_event_put32(&event, 20, selection->time);
		xs_event_send(select->client, &event);
	}
}

// a new owner, or none: the old one hears it lost it, listeners hear, and
// flame hears when an X client has it now
static void set_owner(struct flame_xserver *xserver, struct xs_selection *selection,
		uint32_t owner, struct xs_client *client, uint32_t time, uint8_t why) {
	bool was_x = selection->client != NULL;
	if (selection->owner && selection->owner != owner) {
		struct xs_event event = {0};
		xs_event_put8(&event, 0, XS_EVENT_SELECTION_CLEAR);
		xs_event_put32(&event, 4, xs_now(xserver));
		xs_event_put32(&event, 8, selection->owner);
		xs_event_put32(&event, 12, selection->atom);
		if (selection->client) {
			xs_event_send(selection->client, &event);
		}
	}
	selection->owner = owner;
	selection->client = owner ? client : NULL;
	selection->time = time ? time : xs_now(xserver);
	owner_event(xserver, selection, why);
	// flame knows when it took one itself
	bool flames = owner && !client;
	bool is_x = selection->client != NULL;
	if ((is_x || (was_x && !flames)) && xserver->hooks.selection_owner) {
		xserver->hooks.selection_owner(xserver->hooks.data, selection->atom, is_x);
	}
}

void xs_set_selection_owner(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	uint32_t owner = xin32(in, 4), atom_id = xin32(in, 8), time = xin32(in, 12);
	if (owner && !xserver_window(xserver, owner)) {
		xs_error(client, XS_ERROR_WINDOW, owner, 22, 0);
		return;
	}
	if (!xs_atom_name(&xserver->atoms, atom_id)) {
		xs_error(client, XS_ERROR_ATOM, atom_id, 22, 0);
		return;
	}
	struct xs_selection *selection = find_selection(xserver, atom_id, true);
	if (!selection) {
		xs_error(client, XS_ERROR_ALLOC, 0, 22, 0);
		return;
	}
	// an older claim than the owner's doesn't count
	if (time && selection->owner && time < selection->time) {
		return;
	}
	set_owner(xserver, selection, owner, owner ? client : NULL, time, SET_SELECTION_OWNER);
}

void xs_get_selection_owner(struct xs_client *client, const struct xin *in) {
	struct xs_selection *selection = find_selection(client->xserver, xin32(in, 4), false);
	struct xout out;
	xs_reply_begin(client, &out, 0);
	xout32(&out, selection ? selection->owner : 0);
	xs_reply_send(client, &out);
}

static void selection_notify(struct xs_client *client, uint32_t time, uint32_t requestor,
		uint32_t selection, uint32_t target, uint32_t property) {
	struct xs_event event = {0};
	xs_event_put8(&event, 0, XS_EVENT_SELECTION_NOTIFY);
	xs_event_put32(&event, 4, time);
	xs_event_put32(&event, 8, requestor);
	xs_event_put32(&event, 12, selection);
	xs_event_put32(&event, 16, target);
	xs_event_put32(&event, 20, property);
	xs_event_send(client, &event);
}

void xs_convert_selection(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	uint32_t requestor = xin32(in, 4), atom_id = xin32(in, 8), target = xin32(in, 12);
	uint32_t property = xin32(in, 16), time = xin32(in, 20);
	if (!xserver_window(xserver, requestor)) {
		xs_error(client, XS_ERROR_WINDOW, requestor, 24, 0);
		return;
	}
	struct xs_selection *selection = find_selection(xserver, atom_id, false);
	if (selection && selection->owner && selection->client) {
		struct xs_event event = {0};
		xs_event_put8(&event, 0, XS_EVENT_SELECTION_REQUEST);
		xs_event_put32(&event, 4, time);
		xs_event_put32(&event, 8, selection->owner);
		xs_event_put32(&event, 12, requestor);
		xs_event_put32(&event, 16, atom_id);
		xs_event_put32(&event, 20, target);
		xs_event_put32(&event, 24, property);
		xs_event_send(selection->client, &event);
		return;
	}
	if (selection && selection->owner && xserver->hooks.selection_request) {
		struct xs_selection_request *request = calloc(1, sizeof(*request));
		if (request) {
			request->selection = atom_id;
			request->target = target;
			// obsolete clients leave it out: the target names it then
			request->property = property ? property : target;
			request->requestor = requestor;
			request->time = time;
			wl_list_insert(&xserver->selection_requests, &request->link);
			xserver->hooks.selection_request(xserver->hooks.data, request);
			return;
		}
	}
	// nobody owns it: the answer is no
	selection_notify(client, time, requestor, atom_id, target, 0);
}

// ---- flame owning selections ----

void xserver_selection_own(struct flame_xserver *xserver, uint32_t atom_id, bool own) {
	struct xs_selection *selection = find_selection(xserver, atom_id, true);
	if (!selection) {
		return;
	}
	if (own) {
		set_owner(xserver, selection, XS_FLAME_WINDOW, NULL, 0, SET_SELECTION_OWNER);
	} else if (selection->owner == XS_FLAME_WINDOW) {
		set_owner(xserver, selection, 0, NULL, 0, SET_SELECTION_OWNER);
	}
}

bool xserver_selection_owned(struct flame_xserver *xserver, uint32_t atom_id) {
	struct xs_selection *selection = find_selection(xserver, atom_id, false);
	return selection && selection->owner && selection->client;
}

void xserver_selection_answer(struct flame_xserver *xserver,
		struct xs_selection_request *request, uint32_t type, uint8_t format,
		const void *data, size_t len) {
	struct xs_window *requestor = xserver_window(xserver, request->requestor);
	uint32_t property = 0;
	if (requestor && requestor->owner && type) {
		size_t unit = format / 8;
		uint8_t error;
		if (unit && len % unit == 0 && xs_window_change_property(xserver, requestor, 0,
				request->property, type, format, (uint32_t)(len / unit), data, &error)) {
			property = request->property;
		}
	}
	if (requestor && requestor->owner) {
		selection_notify(requestor->owner, request->time, request->requestor,
			request->selection, request->target, property);
	}
	wl_list_remove(&request->link);
	free(request);
}

// ---- flame asking an owner ----

static void transfer_free(struct xs_transfer *transfer) {
	struct flame_xserver *xserver = transfer->xserver;
	wl_list_remove(&transfer->link);
	if (transfer->timeout) {
		wl_event_source_remove(transfer->timeout);
	}
	struct xs_window *window = transfer->window;
	transfer->window = NULL;
	if (window) {
		xs_window_destroy(xserver, window);
	}
	free(transfer);
}

static void transfer_end(struct xs_transfer *transfer, uint32_t type, const uint8_t *bytes,
		size_t len) {
	xs_transfer_fn fn = transfer->fn;
	void *data = transfer->data;
	transfer_free(transfer);
	fn(data, type, bytes, len, true);
}

static int handle_timeout(void *data) {
	struct xs_transfer *transfer = data;
	wlr_log(WLR_DEBUG, "xserver: selection owner never answered");
	transfer->timeout = NULL;
	transfer_end(transfer, 0, NULL, 0);
	return 0;
}

struct xs_transfer *xserver_selection_convert(struct flame_xserver *xserver,
		uint32_t selection_atom, uint32_t target, xs_transfer_fn fn, void *data) {
	struct xs_selection *selection = find_selection(xserver, selection_atom, false);
	if (!selection || !selection->client) {
		return NULL;
	}
	struct xs_transfer *transfer = calloc(1, sizeof(*transfer));
	if (!transfer) {
		return NULL;
	}
	uint32_t id = FLAME_WINDOWS + (xserver->flame_window_ids++ & 0xffff);
	while (xserver_window(xserver, id)) {
		id = FLAME_WINDOWS + (xserver->flame_window_ids++ & 0xffff);
	}
	transfer->window = xs_window_create(xserver, NULL, id, xserver->root);
	if (!transfer->window) {
		free(transfer);
		return NULL;
	}
	transfer->window->class = 2; // InputOnly
	transfer->xserver = xserver;
	transfer->selection = selection_atom;
	transfer->target = target;
	transfer->property = atom(xserver, "_FLAME_SELECTION");
	transfer->fn = fn;
	transfer->data = data;
	transfer->timeout = wl_event_loop_add_timer(xserver->loop, handle_timeout, transfer);
	if (transfer->timeout) {
		wl_event_source_timer_update(transfer->timeout, TRANSFER_TIMEOUT_MS);
	}
	wl_list_insert(&xserver->transfers, &transfer->link);

	struct xs_event event = {0};
	xs_event_put8(&event, 0, XS_EVENT_SELECTION_REQUEST);
	xs_event_put32(&event, 4, xs_now(xserver));
	xs_event_put32(&event, 8, selection->owner);
	xs_event_put32(&event, 12, id);
	xs_event_put32(&event, 16, selection_atom);
	xs_event_put32(&event, 20, target);
	xs_event_put32(&event, 24, transfer->property);
	xs_event_send(selection->client, &event);
	return transfer;
}

void xserver_transfer_cancel(struct xs_transfer *transfer) {
	transfer_free(transfer);
}

// the owner put a piece in: hand it over, and ask for the next by deleting it
static void take_piece(struct xs_transfer *transfer) {
	struct flame_xserver *xserver = transfer->xserver;
	struct xs_property *property = xs_window_property(transfer->window, transfer->property);
	if (!property) {
		return;
	}
	size_t len = (size_t)property->units * (property->format / 8);
	uint32_t type = property->type;
	if (len == 0) {
		transfer_end(transfer, type, NULL, 0); // the last, empty one
		return;
	}
	uint8_t *piece = malloc(len);
	if (!piece) {
		transfer_end(transfer, 0, NULL, 0);
		return;
	}
	memcpy(piece, property->data, len);
	if (transfer->timeout) {
		wl_event_source_timer_update(transfer->timeout, TRANSFER_TIMEOUT_MS);
	}
	// deleting it asks for the next. last thing to touch transfer: fn may
	// cancel it
	xs_window_delete_property(xserver, transfer->window, transfer->property);
	transfer->fn(transfer->data, type, piece, len, false);
	free(piece);
}

void xs_selection_window_event(struct flame_xserver *xserver, struct xs_window *window,
		const struct xs_event *event) {
	struct xs_transfer *transfer = transfer_of(xserver, window);
	if (!transfer || (event->bytes[0] & 0x7f) != XS_EVENT_SELECTION_NOTIFY || transfer->incr) {
		return;
	}
	uint32_t property_name;
	memcpy(&property_name, event->bytes + 20, 4);
	struct xs_property *property = property_name
		? xs_window_property(window, property_name) : NULL;
	if (!property) {
		transfer_end(transfer, 0, NULL, 0); // refused
		return;
	}
	if (property->type == atom(xserver, "INCR")) {
		// it comes in pieces: deleting the property says go
		transfer->incr = true;
		xs_window_delete_property(xserver, window, property_name);
		return;
	}
	size_t len = (size_t)property->units * (property->format / 8);
	uint8_t *copy = len ? malloc(len) : NULL;
	if (len && !copy) {
		transfer_end(transfer, 0, NULL, 0);
		return;
	}
	if (len) {
		memcpy(copy, property->data, len);
	}
	uint32_t type = property->type;
	xs_window_delete_property(xserver, window, property_name);
	transfer_end(transfer, type, copy, len);
	free(copy);
}

void xs_selection_window_property(struct flame_xserver *xserver, struct xs_window *window,
		uint32_t name) {
	struct xs_transfer *transfer = transfer_of(xserver, window);
	if (transfer && transfer->incr && name == transfer->property) {
		take_piece(transfer);
	}
}

// ---- XFIXES ----

void xs_selection_select_input(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	uint32_t window = xin32(in, 4), selection = xin32(in, 8), mask = xin32(in, 12);
	if (!xserver_window(xserver, window)) {
		xs_error(client, XS_ERROR_WINDOW, window, XS_XFIXES_OPCODE, 2);
		return;
	}
	struct select *select, *tmp;
	wl_list_for_each_safe(select, tmp, &xserver->selection_selects, link) {
		if (select->client == client && select->window == window
				&& select->selection == selection) {
			if (mask) {
				select->mask = mask;
			} else {
				wl_list_remove(&select->link);
				free(select);
			}
			return;
		}
	}
	if (!mask) {
		return;
	}
	select = calloc(1, sizeof(*select));
	if (!select) {
		xs_error(client, XS_ERROR_ALLOC, 0, XS_XFIXES_OPCODE, 2);
		return;
	}
	select->client = client;
	select->window = window;
	select->selection = selection;
	select->mask = mask;
	wl_list_insert(&xserver->selection_selects, &select->link);
}

// ---- going away ----

void xs_selection_window_gone(struct flame_xserver *xserver, struct xs_window *window) {
	// a client took a transfer's window down with its own: it fails, soon
	struct xs_transfer *transfer = transfer_of(xserver, window);
	if (transfer) {
		transfer->window = NULL;
		if (transfer->timeout) {
			wl_event_source_timer_update(transfer->timeout, 1);
		}
	}
	struct xs_selection *selection;
	wl_list_for_each(selection, &xserver->selections, link) {
		if (selection->owner == window->id && selection->client) {
			set_owner(xserver, selection, 0, NULL, 0, SELECTION_WINDOW_DESTROY);
		}
	}
	struct select *select, *tmp;
	wl_list_for_each_safe(select, tmp, &xserver->selection_selects, link) {
		if (select->window == window->id) {
			wl_list_remove(&select->link);
			free(select);
		}
	}
}

void xs_selection_client_gone(struct flame_xserver *xserver, struct xs_client *client) {
	struct xs_selection *selection;
	wl_list_for_each(selection, &xserver->selections, link) {
		if (client && selection->client == client) {
			set_owner(xserver, selection, 0, NULL, 0, SELECTION_CLIENT_CLOSE);
		}
	}
	struct select *select, *tmp;
	wl_list_for_each_safe(select, tmp, &xserver->selection_selects, link) {
		if (!client || select->client == client) {
			wl_list_remove(&select->link);
			free(select);
		}
	}
}

void xs_selection_finish(struct flame_xserver *xserver) {
	// flame let go of its transfers before this
	struct xs_transfer *transfer, *ttmp;
	wl_list_for_each_safe(transfer, ttmp, &xserver->transfers, link) {
		transfer_free(transfer);
	}
	struct xs_selection_request *request, *rtmp;
	wl_list_for_each_safe(request, rtmp, &xserver->selection_requests, link) {
		wl_list_remove(&request->link);
		free(request);
	}
	xs_selection_client_gone(xserver, NULL);
	struct xs_selection *selection, *stmp;
	wl_list_for_each_safe(selection, stmp, &xserver->selections, link) {
		wl_list_remove(&selection->link);
		free(selection);
	}
}
