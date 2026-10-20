#include "xselection/xselection.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

#include "core/server.h"
#include "util/listener.h"
#include "xselection/internal.h"
#include "xserver/xserver.h"

#define UTF8_MIME "text/plain;charset=utf-8"
#define TEXT_MIME "text/plain"
#define MAX_SIZE (256u << 20) // nobody pastes more than this

// an X selection offered to wayland apps: a data source, or a primary
// selection source, with the X target each mime type comes from
struct xsel_source {
	struct wlr_data_source data;
	struct wlr_primary_selection_source primary;
	struct xsel *sel;
	struct wl_array targets; // uint32_t, one per mime type
};

// bytes going from one side to the other: from an X owner into a wayland
// app's fd, or from a wayland source's fd to an X app's request
struct xsel_move {
	struct wl_list link; // flame_xselection.moves
	struct flame_xselection *owner;
	int fd;
	struct wl_event_source *source;
	struct xs_transfer *transfer; // X to wayland, while the X owner sends
	struct xs_selection_request *request; // wayland to X
	uint32_t type; // what the X app gets
	char *buf;
	size_t len, done; // done: written so far (to wayland)
	bool finished; // the X owner sent its last piece
};

static void move_free(struct xsel_move *move) {
	if (move->transfer) {
		xserver_transfer_cancel(move->transfer);
	}
	if (move->source) {
		wl_event_source_remove(move->source);
	}
	if (move->fd >= 0) {
		close(move->fd);
	}
	free(move->buf);
	wl_list_remove(&move->link);
	free(move);
}

static bool move_append(struct xsel_move *move, const void *bytes, size_t len) {
	if (move->len + len > MAX_SIZE) {
		return false;
	}
	char *buf = realloc(move->buf, move->len + len + 1);
	if (!buf) {
		return false;
	}
	memcpy(buf + move->len, bytes, len);
	move->buf = buf;
	move->len += len;
	return true;
}

static struct xsel_move *move_new(struct flame_xselection *xselection, int fd) {
	struct xsel_move *move = calloc(1, sizeof(*move));
	if (!move) {
		close(fd);
		return NULL;
	}
	move->owner = xselection;
	move->fd = fd;
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	wl_list_insert(&xselection->moves, &move->link);
	return move;
}

// STRING is Latin-1, wayland text is UTF-8. Latin-1 to UTF-8 at most
// doubles it
static bool append_latin1_as_utf8(struct xsel_move *move, const uint8_t *bytes, size_t len) {
	char buf[4096];
	while (len) {
		size_t n = 0;
		for (; len && n + 2 <= sizeof(buf); bytes++, len--) {
			if (*bytes < 0x80) {
				buf[n++] = (char)*bytes;
			} else {
				buf[n++] = (char)(0xc0 | *bytes >> 6);
				buf[n++] = (char)(0x80 | (*bytes & 0x3f));
			}
		}
		if (!move_append(move, buf, n)) {
			return false;
		}
	}
	return true;
}

// and back, in place: what Latin-1 can't say is a ?, bytes that aren't
// UTF-8 stay what they were
static size_t utf8_to_latin1(char *text, size_t len) {
	const uint8_t *in = (const uint8_t *)text;
	size_t out = 0;
	for (size_t i = 0; i < len;) {
		uint8_t c = in[i];
		size_t extra = c >= 0xf0 && c < 0xf8 ? 3 : c >= 0xe0 ? 2 : c >= 0xc2 ? 1 : 0;
		bool valid = extra && c < 0xf8 && i + extra < len;
		uint32_t cp = c & (0x3f >> extra);
		for (size_t k = 1; valid && k <= extra; k++) {
			valid = (in[i + k] & 0xc0) == 0x80;
			cp = cp << 6 | (in[i + k] & 0x3f);
		}
		if (!valid) {
			text[out++] = (char)c;
			i++;
			continue;
		}
		text[out++] = cp <= 0xff ? (char)cp : '?';
		i += extra + 1;
	}
	return out;
}

// ---- mime types and atoms ----

const char *xsel_mime_of(struct flame_xselection *xselection, uint32_t target) {
	if (target == xselection->utf8_string) {
		return UTF8_MIME;
	}
	if (target == xselection->text || target == xselection->string) {
		return TEXT_MIME;
	}
	const char *name = xserver_atom_name(xselection->xserver, target);
	// TARGETS, TIMESTAMP, MULTIPLE and their friends aren't data
	return name && strchr(name, '/') ? name : NULL;
}

bool xsel_offers(struct wl_array *mime_types, const char *mime) {
	char **each;
	wl_array_for_each(each, mime_types) {
		if (!strcmp(*each, mime)) {
			return true;
		}
	}
	return false;
}

// ---- X to wayland ----

static void handle_move_writable_end(struct xsel_move *move) {
	if (move->done == move->len && move->finished) {
		move_free(move);
	}
}

static int handle_move_writable(int fd, uint32_t mask, void *data) {
	struct xsel_move *move = data;
	while (move->done < move->len) {
		ssize_t n = write(fd, move->buf + move->done, move->len - move->done);
		if (n < 0 && errno == EAGAIN) {
			return 0;
		}
		if (n <= 0) {
			move_free(move); // the reader went away
			return 0;
		}
		move->done += (size_t)n;
	}
	wl_event_source_fd_update(move->source, 0);
	handle_move_writable_end(move);
	return 0;
}

// the X owner's answer, in pieces or all at once
static void handle_x_answer(void *data, uint32_t type, const uint8_t *bytes, size_t len,
		bool last) {
	struct xsel_move *move = data;
	if (last) {
		move->transfer = NULL;
		move->finished = true;
	}
	bool latin1 = type == move->owner->string;
	if ((len && !(latin1 ? append_latin1_as_utf8(move, bytes, len)
				: move_append(move, bytes, len))) || (last && !type)) {
		move_free(move);
		return;
	}
	if (move->done < move->len) {
		wl_event_source_fd_update(move->source, WL_EVENT_WRITABLE);
	} else {
		handle_move_writable_end(move);
	}
}

void xsel_fetch(struct flame_xselection *xselection, uint32_t selection, uint32_t target,
		int fd) {
	struct xsel_move *move = target ? move_new(xselection, fd) : NULL;
	if (!move) {
		if (!target) {
			close(fd);
		}
		return;
	}
	move->source = wl_event_loop_add_fd(xselection->server->loop, fd, 0,
		handle_move_writable, move);
	move->transfer = xserver_selection_convert(xselection->xserver, selection, target,
		handle_x_answer, move);
	if (!move->source || !move->transfer) {
		move_free(move);
	}
}

uint32_t xsel_target_of(struct wl_array *mimes, struct wl_array *targets, const char *mime) {
	size_t i = 0;
	char **each;
	wl_array_for_each(each, mimes) {
		if (!strcmp(*each, mime)) {
			return ((uint32_t *)targets->data)[i];
		}
		i++;
	}
	return 0;
}

static void source_send(struct xsel_source *source, const char *mime, int fd) {
	if (!source->sel) {
		close(fd); // flame is going down
		return;
	}
	struct wl_array *mimes = source->sel->primary ? &source->primary.mime_types
		: &source->data.mime_types;
	xsel_fetch(source->sel->owner, source->sel->atom,
		xsel_target_of(mimes, &source->targets, mime), fd);
}

static void source_free(struct xsel_source *source) {
	if (source->sel && source->sel->source == source) {
		source->sel->source = NULL;
	}
	wl_array_release(&source->targets);
	free(source);
}

static void data_send(struct wlr_data_source *data, const char *mime, int32_t fd) {
	struct xsel_source *source = wl_container_of(data, source, data);
	source_send(source, mime, fd);
}

static void data_destroy(struct wlr_data_source *data) {
	struct xsel_source *source = wl_container_of(data, source, data);
	source_free(source);
}

static const struct wlr_data_source_impl data_impl = {
	.send = data_send,
	.destroy = data_destroy,
};

static void primary_send(struct wlr_primary_selection_source *primary, const char *mime,
		int fd) {
	struct xsel_source *source = wl_container_of(primary, source, primary);
	source_send(source, mime, fd);
}

static void primary_destroy(struct wlr_primary_selection_source *primary) {
	struct xsel_source *source = wl_container_of(primary, source, primary);
	source_free(source);
}

static const struct wlr_primary_selection_source_impl primary_impl = {
	.send = primary_send,
	.destroy = primary_destroy,
};

static bool is_ours_data(struct wlr_data_source *data) {
	return data && data->impl == &data_impl;
}

static bool is_ours_primary(struct wlr_primary_selection_source *primary) {
	return primary && primary->impl == &primary_impl;
}

bool xsel_add_offer(struct wl_array *mimes, struct wl_array *targets, const char *mime,
		uint32_t target) {
	if (xsel_offers(mimes, mime)) {
		return true;
	}
	char **slot = wl_array_add(mimes, sizeof(*slot));
	uint32_t *t = wl_array_add(targets, sizeof(*t));
	if (!slot || !t || !(*slot = strdup(mime))) {
		return false;
	}
	*t = target;
	return true;
}

// the X owner's TARGETS: wayland gets a source offering the same
static void handle_targets(void *data, uint32_t type, const uint8_t *bytes, size_t len,
		bool last) {
	struct xsel *sel = data;
	struct flame_xselection *xselection = sel->owner;
	if (!last) {
		return; // nobody sends TARGETS in pieces
	}
	sel->targets = NULL;
	struct xsel_source *source = calloc(1, sizeof(*source));
	if (!source) {
		return;
	}
	source->sel = sel;
	wl_array_init(&source->targets);
	struct wl_array *mimes;
	if (sel->primary) {
		wlr_primary_selection_source_init(&source->primary, &primary_impl);
		mimes = &source->primary.mime_types;
	} else {
		wlr_data_source_init(&source->data, &data_impl);
		mimes = &source->data.mime_types;
	}
	bool ok = true;
	if (type == xselection->atom && len % 4 == 0) {
		const uint32_t *targets = (const uint32_t *)bytes;
		for (size_t i = 0; i < len / 4; i++) {
			const char *mime = xsel_mime_of(xselection, targets[i]);
			if (mime) {
				ok = ok && xsel_add_offer(mimes, &source->targets, mime, targets[i]);
			}
		}
	} else {
		// it doesn't say: text is a safe bet
		ok = xsel_add_offer(mimes, &source->targets, UTF8_MIME, xselection->utf8_string)
			&& xsel_add_offer(mimes, &source->targets, TEXT_MIME, xselection->string);
	}
	if (!ok || mimes->size == 0) {
		if (sel->primary) {
			wlr_primary_selection_source_destroy(&source->primary);
		} else {
			wlr_data_source_destroy(&source->data);
		}
		return;
	}
	sel->source = source;
	struct flame_server *server = xselection->server;
	uint32_t serial = wl_display_next_serial(server->display);
	if (sel->primary) {
		wlr_seat_set_primary_selection(server->input.seat, &source->primary, serial);
	} else {
		wlr_seat_set_selection(server->input.seat, &source->data, serial);
	}
}

static struct xsel *sel_of(struct flame_xselection *xselection, uint32_t atom) {
	if (atom == xselection->clipboard.atom) {
		return &xselection->clipboard;
	}
	if (atom == xselection->primary.atom) {
		return &xselection->primary;
	}
	return NULL;
}

void xselection_owner_changed(struct flame_xselection *xselection, uint32_t selection,
		bool x_owned) {
	if (xsel_dnd_owner_changed(xselection, selection, x_owned)) {
		return;
	}
	struct xsel *sel = sel_of(xselection, selection);
	if (!sel) {
		return;
	}
	if (sel->targets) {
		xserver_transfer_cancel(sel->targets);
		sel->targets = NULL;
	}
	if (x_owned) {
		sel->targets = xserver_selection_convert(xselection->xserver, selection,
			xselection->targets, handle_targets, sel);
		return;
	}
	// the X owner let go: what wayland had from it goes too
	struct flame_server *server = xselection->server;
	struct wlr_seat *seat = server->input.seat;
	uint32_t serial = wl_display_next_serial(server->display);
	if (sel->primary && is_ours_primary(seat->primary_selection_source)) {
		wlr_seat_set_primary_selection(seat, NULL, serial);
	} else if (!sel->primary && is_ours_data(seat->selection_source)) {
		wlr_seat_set_selection(seat, NULL, serial);
	}
}

// ---- wayland to X ----

static int handle_move_readable(int fd, uint32_t mask, void *data) {
	struct xsel_move *move = data;
	struct flame_xselection *xselection = move->owner;
	char buf[65536];
	for (;;) {
		ssize_t n = read(fd, buf, sizeof(buf));
		if (n < 0 && errno == EAGAIN) {
			return 0;
		}
		if (n > 0 && move_append(move, buf, (size_t)n)) {
			continue;
		}
		// the end, or it broke: whatever came is the answer
		bool ok = n == 0;
		if (ok && move->type == xselection->string) {
			move->len = utf8_to_latin1(move->buf, move->len);
		}
		xserver_selection_answer(xselection->xserver, move->request, ok ? move->type : 0, 8,
			move->buf, move->len);
		move->request = NULL;
		move_free(move);
		return 0;
	}
}

// what wayland mime type answers an X target, NULL for none
static const char *wanted_mime(struct flame_xselection *xselection, struct wl_array *mimes,
		uint32_t target) {
	if (target == xselection->utf8_string || target == xselection->text
			|| target == xselection->string) {
		if (xsel_offers(mimes, UTF8_MIME)) {
			return UTF8_MIME;
		}
		if (xsel_offers(mimes, TEXT_MIME)) {
			return TEXT_MIME;
		}
		return xsel_offers(mimes, "UTF8_STRING") ? "UTF8_STRING" : NULL;
	}
	const char *name = xserver_atom_name(xselection->xserver, target);
	return name && xsel_offers(mimes, name) ? name : NULL;
}

static void answer_targets(struct flame_xselection *xselection,
		struct xs_selection_request *request, struct wl_array *mimes) {
	uint32_t targets[256];
	size_t len = 0;
	targets[len++] = xselection->targets;
	targets[len++] = xselection->timestamp;
	if (xsel_offers(mimes, UTF8_MIME) || xsel_offers(mimes, TEXT_MIME)) {
		targets[len++] = xselection->utf8_string;
		targets[len++] = xselection->string;
		targets[len++] = xselection->text;
	}
	char **each;
	wl_array_for_each(each, mimes) {
		if (len < 256 && strchr(*each, '/')) {
			targets[len++] = xserver_atom(xselection->xserver, *each);
		}
	}
	xserver_selection_answer(xselection->xserver, request, xselection->atom, 32, targets,
		len * 4);
}

static void send_data(void *source, const char *mime, int fd) {
	wlr_data_source_send(source, mime, fd);
}

static void send_primary(void *source, const char *mime, int fd) {
	wlr_primary_selection_source_send(source, mime, fd);
}

void xselection_request(struct flame_xselection *xselection,
		struct xs_selection_request *request) {
	if (xselection_dnd_request(xselection, request)) {
		return;
	}
	struct xsel *sel = sel_of(xselection, request->selection);
	struct wlr_seat *seat = xselection->server->input.seat;
	struct wlr_data_source *data = sel && !sel->primary ? seat->selection_source : NULL;
	struct wlr_primary_selection_source *primary = sel && sel->primary
		? seat->primary_selection_source : NULL;
	if (is_ours_data(data) || is_ours_primary(primary)) {
		data = NULL;
		primary = NULL;
	}
	if (data) {
		xsel_answer(xselection, request, &data->mime_types, send_data, data);
	} else if (primary) {
		xsel_answer(xselection, request, &primary->mime_types, send_primary, primary);
	} else {
		xserver_selection_answer(xselection->xserver, request, 0, 8, NULL, 0);
	}
}

void xsel_answer(struct flame_xselection *xselection, struct xs_selection_request *request,
		struct wl_array *mimes, void (*send)(void *source, const char *mime, int fd),
		void *source) {
	if (request->target == xselection->targets) {
		answer_targets(xselection, request, mimes);
		return;
	}
	if (request->target == xselection->timestamp) {
		uint32_t time = request->time;
		xserver_selection_answer(xselection->xserver, request, xselection->integer, 32, &time,
			4);
		return;
	}
	const char *mime = wanted_mime(xselection, mimes, request->target);
	int fds[2];
	if (!mime || pipe(fds) < 0) {
		xserver_selection_answer(xselection->xserver, request, 0, 8, NULL, 0);
		return;
	}
	fcntl(fds[0], F_SETFD, FD_CLOEXEC);
	fcntl(fds[1], F_SETFD, FD_CLOEXEC);
	struct xsel_move *move = move_new(xselection, fds[0]);
	if (!move) {
		close(fds[1]);
		xserver_selection_answer(xselection->xserver, request, 0, 8, NULL, 0);
		return;
	}
	move->request = request;
	move->type = request->target == xselection->text ? xselection->utf8_string
		: request->target;
	move->source = wl_event_loop_add_fd(xselection->server->loop, fds[0], WL_EVENT_READABLE,
		handle_move_readable, move);
	if (!move->source) {
		close(fds[1]);
		xserver_selection_answer(xselection->xserver, request, 0, 8, NULL, 0);
		move->request = NULL;
		move_free(move);
		return;
	}
	send(source, mime, fds[1]); // the source owns the write end from here
}

// a wayland app copied something (or the copy went away): flame holds it for
// X apps. flame's own offers of X selections are the X side's already
static void handle_set_selection(struct wl_listener *listener, void *data) {
	struct flame_xselection *xselection =
		wl_container_of(listener, xselection, set_selection);
	struct wlr_data_source *source = xselection->server->input.seat->selection_source;
	if (!is_ours_data(source)) {
		xserver_selection_own(xselection->xserver, xselection->clipboard.atom, source != NULL);
	}
}

static void handle_set_primary_selection(struct wl_listener *listener, void *data) {
	struct flame_xselection *xselection =
		wl_container_of(listener, xselection, set_primary_selection);
	struct wlr_primary_selection_source *source =
		xselection->server->input.seat->primary_selection_source;
	if (!is_ours_primary(source)) {
		xserver_selection_own(xselection->xserver, xselection->primary.atom, source != NULL);
	}
}

// ---- setup ----

bool xselection_init(struct flame_xselection *xselection, struct flame_server *server,
		struct flame_xserver *xserver) {
	xselection->server = server;
	xselection->xserver = xserver;
	wl_list_init(&xselection->moves);
	xselection->clipboard = (struct xsel){
		.owner = xselection,
		.atom = xserver_atom(xserver, "CLIPBOARD"),
	};
	xselection->primary = (struct xsel){
		.owner = xselection,
		.atom = xserver_atom(xserver, "PRIMARY"),
		.primary = true,
	};
	xselection->targets = xserver_atom(xserver, "TARGETS");
	xselection->timestamp = xserver_atom(xserver, "TIMESTAMP");
	xselection->utf8_string = xserver_atom(xserver, "UTF8_STRING");
	xselection->text = xserver_atom(xserver, "TEXT");
	xselection->string = xserver_atom(xserver, "STRING");
	xselection->atom = xserver_atom(xserver, "ATOM");
	xselection->integer = xserver_atom(xserver, "INTEGER");
	struct wlr_seat *seat = server->input.seat;
	listener_connect(&xselection->set_selection, &seat->events.set_selection,
		handle_set_selection);
	listener_connect(&xselection->set_primary_selection, &seat->events.set_primary_selection,
		handle_set_primary_selection);
	if (!xsel_dnd_init(xselection)) {
		listener_disconnect(&xselection->set_selection);
		listener_disconnect(&xselection->set_primary_selection);
		return false;
	}
	return true;
}

void xselection_finish(struct flame_xselection *xselection) {
	xsel_dnd_finish(xselection);
	listener_disconnect(&xselection->set_selection);
	listener_disconnect(&xselection->set_primary_selection);
	struct xsel *sels[] = {&xselection->clipboard, &xselection->primary};
	for (int i = 0; i < 2; i++) {
		if (sels[i]->targets) {
			xserver_transfer_cancel(sels[i]->targets);
			sels[i]->targets = NULL;
		}
		if (sels[i]->source) {
			sels[i]->source->sel = NULL;
		}
	}
	struct xsel_move *move, *tmp;
	wl_list_for_each_safe(move, tmp, &xselection->moves, link) {
		if (move->request) {
			move->request = NULL; // the X server frees its requests
		}
		move_free(move);
	}
}
