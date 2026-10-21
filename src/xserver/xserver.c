// accept4, struct ucred
#define _GNU_SOURCE
#include "xserver/xserver.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <wlr/util/log.h>

#include "xserver/internal.h"

// a request bigger than this (BIG-REQUESTS) is garbage, not a window title
#define MAX_REQUEST (16u << 20)
// a client this far behind on reading its replies is stuck, drop it
#define MAX_PENDING (8u << 20)
#define VENDOR "flamewm"

static bool host_little_endian(void) {
	const uint16_t one = 1;
	uint8_t first;
	memcpy(&first, &one, 1);
	return first == 1;
}

uint32_t xs_now(struct flame_xserver *xserver) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

// ---- talking back ----

// fds ride along with the first bytes that go out after they were queued.
// xcb keeps received fds in order until the reply that takes them shows up
static ssize_t send_with_fds(struct xs_client *client) {
	struct iovec iov = {client->out.data, client->out.len};
	struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
	union {
		char buf[CMSG_SPACE(sizeof(int) * 16)];
		struct cmsghdr align;
	} control;
	if (client->out_fds_len > 0) {
		size_t size = sizeof(int) * (size_t)client->out_fds_len;
		msg.msg_control = control.buf;
		msg.msg_controllen = CMSG_SPACE(size);
		struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		cmsg->cmsg_len = CMSG_LEN(size);
		memcpy(CMSG_DATA(cmsg), client->out_fds, size);
	}
	ssize_t sent = sendmsg(client->fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
	if (sent > 0) {
		for (int i = 0; i < client->out_fds_len; i++) {
			close(client->out_fds[i]);
		}
		client->out_fds_len = 0;
	}
	return sent;
}

int xs_client_take_fd(struct xs_client *client) {
	if (client->in_fds_len == 0) {
		return -1;
	}
	int fd = client->in_fds[0];
	memmove(client->in_fds, client->in_fds + 1,
		sizeof(int) * (size_t)--client->in_fds_len);
	return fd;
}

bool xs_client_send_fd(struct xs_client *client, int fd) {
	if (client->out_fds_len == (int)(sizeof(client->out_fds) / sizeof(client->out_fds[0]))) {
		close(fd);
		return false;
	}
	client->out_fds[client->out_fds_len++] = fd;
	return true;
}

static void flush(struct xs_client *client) {
	while (client->out.len > 0 && !client->dead) {
		ssize_t sent = send_with_fds(client);
		if (sent < 0) {
			if (errno == EINTR) {
				continue;
			}
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				client->dead = true; client->why = "send failed";
			}
			break;
		}
		strbuf_consume(&client->out, (size_t)sent);
	}
	if (!client->dead) {
		wl_event_source_fd_update(client->source,
			WL_EVENT_READABLE | (client->out.len ? WL_EVENT_WRITABLE : 0));
	}
}

void xs_client_send(struct xs_client *client, const void *data, size_t len) {
	if (client->dead) {
		return;
	}
	strbuf_append(&client->out, data, len);
	if (client->out.failed || client->out.len > MAX_PENDING) {
		wlr_log(WLR_INFO, "xserver: client %d (%s) isn't reading, dropping it",
			(int)client->pid, client->comm);
		client->dead = true; client->why = "output overflow";
		return;
	}
	// replies go out when the batch of requests is done (see handle_client).
	// events flame sends on its own (input, Present completes, focus) come
	// outside any batch: the socket being writable wakes handle_client to
	// send them right away, or they'd wait for the app's next request
	wl_event_source_fd_update(client->source, WL_EVENT_READABLE | WL_EVENT_WRITABLE);
}

void xs_error(struct xs_client *client, uint8_t code, uint32_t value, uint8_t major,
		uint16_t minor) {
	wlr_log(WLR_DEBUG, "xserver: error %u for %s (%d), request %u.%u, value 0x%x",
		code, client->comm, (int)client->pid, major, minor, value);
	uint8_t error[32] = {0, code};
	memcpy(error + 2, &client->seq, 2);
	memcpy(error + 4, &value, 4);
	memcpy(error + 8, &minor, 2);
	error[10] = major;
	xs_client_send(client, error, sizeof(error));
}

void xs_reply_begin(struct xs_client *client, struct xout *out, uint8_t data) {
	xout_init(out, false);
	xout8(out, 1);
	xout8(out, data);
	xout16(out, client->seq);
	xout32(out, 0); // length, filled in by xs_reply_send
}

void xs_reply_send(struct xs_client *client, struct xout *out) {
	// at least 32 bytes, whole units of 4 after that
	if (out->buf.len < 32) {
		xout_zero(out, 32 - out->buf.len);
	}
	xout_pad(out);
	if (out->buf.failed) {
		xs_error(client, XS_ERROR_ALLOC, 0, 0, 0);
		xout_finish(out);
		return;
	}
	uint32_t length = (uint32_t)((out->buf.len - 32) / 4);
	memcpy(out->buf.data + 4, &length, 4);
	xs_client_send(client, out->buf.data, out->buf.len);
	xout_finish(out);
}

void xs_event_put8(struct xs_event *event, size_t at, uint8_t v) {
	event->bytes[at] = v;
}

void xs_event_put16(struct xs_event *event, size_t at, uint16_t v) {
	memcpy(event->bytes + at, &v, 2);
}

void xs_event_put32(struct xs_event *event, size_t at, uint32_t v) {
	memcpy(event->bytes + at, &v, 4);
}

void xs_event_send(struct xs_client *client, const struct xs_event *event) {
	struct xs_event copy = *event;
	// KeymapNotify has no sequence number, nothing here sends one
	memcpy(copy.bytes + 2, &client->seq, 2);
	xs_client_send(client, copy.bytes, sizeof(copy.bytes));
}

void xs_event_deliver(struct flame_xserver *xserver, struct xs_window *window, uint32_t mask,
		const struct xs_event *event) {
	struct xs_interest *interest;
	wl_list_for_each(interest, &window->interests, link) {
		if (interest->mask & mask) {
			xs_event_send(interest->client, event);
		}
	}
}

void xs_event_send_input(struct xs_client *client, const struct xs_event *event) {
	// key, button, motion and crossing events carry the state at 28, the
	// layout group in it only for clients that know XKB
	uint8_t type = event->bytes[0] & 0x7f;
	if (type >= 2 && type <= 8 && !client->xkb) {
		struct xs_event copy = *event;
		uint16_t state;
		memcpy(&state, copy.bytes + 28, 2);
		state &= (uint16_t)~0x6000;
		memcpy(copy.bytes + 28, &state, 2);
		xs_event_send(client, &copy);
	} else {
		xs_event_send(client, event);
	}
}

void xs_event_deliver_input(struct flame_xserver *xserver, struct xs_window *window,
		uint32_t mask, int evtype, const struct xs_event *event, bool repeat_release,
		struct xs_client *only) {
	struct xs_interest *interest;
	wl_list_for_each(interest, &window->interests, link) {
		struct xs_client *client = interest->client;
		if (!(interest->mask & mask) || xs_xi_client_selected(client, window, evtype)
				|| (repeat_release && client->detectable_repeat)
				|| (only && client != only)) {
			continue;
		}
		xs_event_send_input(client, event);
	}
}

bool xs_event_wanted_input(struct xs_window *window, uint32_t mask, int evtype) {
	struct xs_interest *interest;
	wl_list_for_each(interest, &window->interests, link) {
		if ((interest->mask & mask)
				&& !xs_xi_client_selected(interest->client, window, evtype)) {
			return true;
		}
	}
	return false;
}

// ---- telling flame ----

static void handle_changed_idle(void *data) {
	struct flame_xserver *xserver = data;
	xserver->changed_idle = NULL;
	if (!xserver->hooks.windows_changed) {
		return;
	}
	xserver->notifying = true;
	xserver->hooks.windows_changed(xserver->hooks.data);
	xserver->notifying = false;
	// what flame did about it changed things again: that's heard next time
	// round the loop. an idle added from an idle runs in the same dispatch,
	// a back and forth there would never let the loop sleep
	if (xserver->changed_again) {
		xserver->changed_again = false;
		wl_event_source_timer_update(xserver->changed_timer, 1);
	}
}

static int handle_changed_timer(void *data) {
	handle_changed_idle(data);
	return 0;
}

void xs_changed(struct flame_xserver *xserver) {
	if (xserver->notifying) {
		xserver->changed_again = true;
		return;
	}
	if (!xserver->changed_idle) {
		xserver->changed_idle =
			wl_event_loop_add_idle(xserver->loop, handle_changed_idle, xserver);
	}
}

// ---- connection setup ----

static void setup_refuse(struct xs_client *client, const char *reason) {
	struct xout out;
	xout_init(&out, false);
	size_t len = strlen(reason);
	xout8(&out, 0); // failed
	xout8(&out, (uint8_t)len);
	xout16(&out, 11);
	xout16(&out, 0);
	xout16(&out, (uint16_t)(xpad(len) / 4));
	xout_bytes(&out, reason, len);
	xout_pad(&out);
	if (!out.buf.failed) {
		xs_client_send(client, out.buf.data, out.buf.len);
	}
	xout_finish(&out);
	flush(client);
	client->dead = true; client->why = "setup refused";
}

static void put_visual(struct xout *out, uint32_t id) {
	xout32(out, id);
	xout8(out, 4); // TrueColor
	xout8(out, 8); // bits per rgb value
	xout16(out, 256); // colormap entries
	xout32(out, 0xff0000);
	xout32(out, 0x00ff00);
	xout32(out, 0x0000ff);
	xout32(out, 0);
}

static void setup_accept(struct xs_client *client) {
	struct flame_xserver *xserver = client->xserver;
	struct xs_window *root = xserver->root;
	struct xout out;
	xout_init(&out, false);
	xout8(&out, 1); // success
	xout8(&out, 0);
	xout16(&out, 11);
	xout16(&out, 0);
	xout16(&out, 0); // length, filled in below
	xout32(&out, 1); // release
	xout32(&out, client->id_base);
	xout32(&out, 0x1fffff); // resource id mask
	xout32(&out, 256); // motion buffer
	xout16(&out, (uint16_t)strlen(VENDOR));
	xout16(&out, 0xffff); // maximum request length, in units
	xout8(&out, 1); // screens
	xout8(&out, 5); // pixmap formats
	xout8(&out, 0); // image byte order: lsb first
	xout8(&out, 0); // bitmap bit order
	xout8(&out, 32); // scanline unit
	xout8(&out, 32); // scanline pad
	xout8(&out, 8); // min keycode
	xout8(&out, 255); // max keycode
	xout32(&out, 0);
	xout_bytes(&out, VENDOR, strlen(VENDOR));
	xout_pad(&out);
	// depth 8: RENDER's alpha masks. 4: libXrender won't use RENDER without
	static const uint8_t formats[5][2] = {{1, 1}, {4, 8}, {8, 8}, {24, 32}, {32, 32}};
	for (int i = 0; i < 5; i++) {
		xout8(&out, formats[i][0]);
		xout8(&out, formats[i][1]);
		xout8(&out, 32);
		xout_zero(&out, 5);
	}
	// the screen
	xout32(&out, XS_ROOT);
	xout32(&out, XS_COLORMAP);
	xout32(&out, 0xffffff); // white
	xout32(&out, 0); // black
	xout32(&out, xs_window_all_masks(root));
	xout16(&out, root->width);
	xout16(&out, root->height);
	// 96 dpi
	xout16(&out, (uint16_t)(root->width * 254 / 960));
	xout16(&out, (uint16_t)(root->height * 254 / 960));
	xout16(&out, 1); // min installed maps
	xout16(&out, 1); // max installed maps
	xout32(&out, XS_VISUAL_24);
	xout8(&out, 0); // backing stores: never
	xout8(&out, 0); // save unders
	xout8(&out, 24); // root depth
	xout8(&out, 5); // allowed depths
	// depth 24, depth 32 (argb windows), depth 1 (bitmaps), 4 and 8 (alpha
	// masks) without visuals
	xout8(&out, 24);
	xout8(&out, 0);
	xout16(&out, 1);
	xout32(&out, 0);
	put_visual(&out, XS_VISUAL_24);
	xout8(&out, 32);
	xout8(&out, 0);
	xout16(&out, 1);
	xout32(&out, 0);
	put_visual(&out, XS_VISUAL_32);
	xout8(&out, 1);
	xout8(&out, 0);
	xout16(&out, 0);
	xout32(&out, 0);
	xout8(&out, 8);
	xout8(&out, 0);
	xout16(&out, 0);
	xout32(&out, 0);
	xout8(&out, 4);
	xout8(&out, 0);
	xout16(&out, 0);
	xout32(&out, 0);

	if (out.buf.failed) {
		xout_finish(&out);
		setup_refuse(client, "out of memory");
		return;
	}
	uint16_t length = (uint16_t)((out.buf.len - 8) / 4);
	memcpy(out.buf.data + 6, &length, 2);
	xs_client_send(client, out.buf.data, out.buf.len);
	xout_finish(&out);
	client->setup_done = true;
}

// the first bytes: byte order, version, authorization nobody checks (the
// socket only lets this user in)
static size_t handle_setup(struct xs_client *client) {
	struct strbuf *in = &client->in;
	if (in->len < 12) {
		return 0;
	}
	const uint8_t *p = (const uint8_t *)in->data;
	uint16_t name_len, data_len;
	memcpy(&name_len, p + 6, 2);
	memcpy(&data_len, p + 8, 2);
	bool little = p[0] == 'l';
	if (little != host_little_endian()) {
		// nothing on this machine talks the other byte order
		setup_refuse(client, "only native byte order clients");
		return in->len;
	}
	size_t need = 12 + xpad(name_len) + xpad(data_len);
	if (in->len < need) {
		return 0;
	}
	setup_accept(client);
	return need;
}

// ---- requests ----

// whole requests at the front of the input, as many as there are
static void handle_requests(struct xs_client *client) {
	struct strbuf *in = &client->in;
	size_t done = 0;
	if (!client->setup_done) {
		done = handle_setup(client);
		if (!client->setup_done) {
			strbuf_consume(in, done);
			return;
		}
	}
	while (!client->dead && in->len - done >= 4) {
		const uint8_t *p = (const uint8_t *)in->data + done;
		uint16_t units16;
		memcpy(&units16, p + 2, 2);
		size_t size, header = 4;
		if (units16 == 0) {
			// BIG-REQUESTS: the real length follows
			if (!client->big_requests) {
				client->seq++;
				xs_error(client, XS_ERROR_LENGTH, 0, p[0], 0);
				client->dead = true; client->why = "big request without BIG-REQUESTS";
				break;
			}
			if (in->len - done < 8) {
				break;
			}
			uint32_t units32;
			memcpy(&units32, p + 4, 4);
			size = (size_t)units32 * 4;
			header = 8;
		} else {
			size = (size_t)units16 * 4;
		}
		if (size < header || size > MAX_REQUEST) {
			client->seq++;
			xs_error(client, XS_ERROR_LENGTH, 0, p[0], 0);
			client->dead = true; client->why = "bad request length";
			break;
		}
		if (in->len - done < size) {
			break;
		}
		client->seq++;
		// handlers see the classic layout: 4 byte header then the body. a
		// big request's extra length word is cut out of the copy they read
		struct xin request;
		uint8_t *copy = NULL;
		if (header == 8) {
			copy = malloc(size - 4);
			if (!copy) {
				xs_error(client, XS_ERROR_ALLOC, 0, p[0], 0);
				done += size;
				continue;
			}
			memcpy(copy, p, 4);
			memcpy(copy + 4, p + 8, size - 8);
			request = (struct xin){copy, size - 4, false};
		} else {
			request = (struct xin){p, size, false};
		}
		xs_request(client, p[0], &request);
		free(copy);
		done += size;
	}
	strbuf_consume(in, done);
}

// ---- clients ----

static void client_free(struct xs_client *client) {
	struct flame_xserver *xserver = client->xserver;
	wlr_log(WLR_DEBUG, "xserver: client %d (%s) base 0x%x gone after %u requests: %s",
		(int)client->pid, client->comm, client->id_base, client->seq,
		client->why ? client->why : "closed");
	xs_client_resources_free(xserver, client);
	if (client->announced && xserver->hooks.disconnected) {
		xserver->hooks.disconnected(xserver->hooks.data, client);
	}
	xserver->slots[client->slot / 64] &= ~(1ull << (client->slot % 64));
	for (int i = 0; i < client->in_fds_len; i++) {
		close(client->in_fds[i]);
	}
	for (int i = 0; i < client->out_fds_len; i++) {
		close(client->out_fds[i]);
	}
	wl_event_source_remove(client->source);
	close(client->fd);
	wl_list_remove(&client->link);
	strbuf_finish(&client->in);
	strbuf_finish(&client->out);
	free(client);
}

static int handle_client(int fd, uint32_t mask, void *data) {
	struct xs_client *client = data;
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		client->dead = true; client->why = "hangup";
	}
	if (!client->dead && (mask & WL_EVENT_READABLE)) {
		char buf[65536];
		struct iovec iov = {buf, sizeof(buf)};
		union {
			char buf[CMSG_SPACE(sizeof(int) * 32)];
			struct cmsghdr align;
		} control;
		struct msghdr msg = {
			.msg_iov = &iov,
			.msg_iovlen = 1,
			.msg_control = control.buf,
			.msg_controllen = sizeof(control.buf),
		};
		ssize_t n = recvmsg(fd, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
		for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); n > 0 && cmsg;
				cmsg = CMSG_NXTHDR(&msg, cmsg)) {
			if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
				continue;
			}
			int count = (int)((cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int));
			int *fds = (int *)CMSG_DATA(cmsg);
			for (int i = 0; i < count; i++) {
				if (client->in_fds_len < (int)(sizeof(client->in_fds) / sizeof(int))) {
					client->in_fds[client->in_fds_len++] = fds[i];
				} else {
					close(fds[i]); // nobody sends this many without asking for them
				}
			}
		}
		if (n == 0) {
			client->dead = true; client->why = "peer closed";
		} else if (n < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
				client->dead = true; client->why = "recv failed";
			}
		} else {
			strbuf_append(&client->in, buf, (size_t)n);
			if (client->in.failed) {
				client->dead = true; client->why = "input overflow";
			} else {
				handle_requests(client);
			}
		}
	}
	if (!client->dead) {
		flush(client);
	}
	if (client->dead) {
		client_free(client);
	}
	return 0;
}

static int free_slot(struct flame_xserver *xserver) {
	// slot 0 would collide with the server's own resources
	for (int slot = 0; slot < 255; slot++) {
		if (!(xserver->slots[slot / 64] & (1ull << (slot % 64)))) {
			return slot;
		}
	}
	return -1;
}

static int handle_accept(int fd, uint32_t mask, void *data) {
	struct flame_xserver *xserver = data;
	int client_fd = accept4(fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
	if (client_fd < 0) {
		return 0;
	}
	struct ucred cred;
	socklen_t cred_len = sizeof(cred);
	if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) < 0
			|| cred.uid != getuid()) {
		// abstract sockets have no file permissions to keep others out
		close(client_fd);
		return 0;
	}
	struct xs_client *client = calloc(1, sizeof(*client));
	int slot = free_slot(xserver);
	if (!client || slot < 0) {
		free(client);
		close(client_fd);
		return 0;
	}
	client->xserver = xserver;
	client->fd = client_fd;
	client->pid = cred.pid;
	client->slot = slot;
	client->id_base = (uint32_t)(slot + 1) << 21;
	strbuf_init(&client->in);
	strbuf_init(&client->out);
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/comm", (int)cred.pid);
	FILE *comm = fopen(path, "re");
	if (comm) {
		if (fgets(client->comm, sizeof(client->comm), comm)) {
			client->comm[strcspn(client->comm, "\n")] = '\0';
		}
		fclose(comm);
	}
	client->source = wl_event_loop_add_fd(xserver->loop, client_fd, WL_EVENT_READABLE,
		handle_client, client);
	if (!client->source) {
		strbuf_finish(&client->in);
		strbuf_finish(&client->out);
		free(client);
		close(client_fd);
		return 0;
	}
	xserver->slots[slot / 64] |= 1ull << (slot % 64);
	wl_list_insert(xserver->clients.prev, &client->link);

	if (xserver->hooks.allow && !xserver->hooks.allow(xserver->hooks.data, client->pid,
			client->comm)) {
		wlr_log(WLR_DEBUG, "xserver: turned away %d (%s)", (int)client->pid, client->comm);
		// xlib reads the reason right after sending its setup
		setup_refuse(client, "flamewm's X only serves apps that draw over wayland");
		client_free(client);
		return 0;
	}
	wlr_log(WLR_DEBUG, "xserver: client %d (%s) base 0x%x", (int)client->pid, client->comm,
		client->id_base);
	client->announced = true;
	if (xserver->hooks.connected) {
		xserver->hooks.connected(xserver->hooks.data, client);
	}
	return 0;
}

// ---- the server ----

void xserver_set_glx_vendor(struct flame_xserver *xserver, const char *vendor) {
	snprintf(xserver->glx_vendor, sizeof(xserver->glx_vendor), "%s", vendor);
}

bool xserver_init(struct flame_xserver *xserver, struct wl_event_loop *loop,
		struct xs_hooks hooks, int width, int height) {
	*xserver = (struct flame_xserver){
		.loop = loop,
		.hooks = hooks,
		.display = -1,
		.fds = {-1, -1},
		.glx_vendor = "mesa",
	};
	wl_list_init(&xserver->clients);
	wl_list_init(&xserver->windows);
	wl_list_init(&xserver->pixmaps);
	wl_list_init(&xserver->fences);
	wl_list_init(&xserver->syncobjs);
	wl_list_init(&xserver->regions);
	wl_list_init(&xserver->present_selects);
	wl_list_init(&xserver->selections);
	wl_list_init(&xserver->transfers);
	wl_list_init(&xserver->selection_requests);
	wl_list_init(&xserver->selection_selects);
	wl_list_init(&xserver->gcs);
	wl_list_init(&xserver->cursors);
	wl_list_init(&xserver->fonts);
	wl_list_init(&xserver->shm_segments);
	wl_list_init(&xserver->pictures);
	wl_list_init(&xserver->glyphset_ids);
	wl_list_init(&xserver->randr_selects);
	xserver->changed_timer = wl_event_loop_add_timer(loop, handle_changed_timer, xserver);
	if (!xserver->changed_timer) {
		return false;
	}
	wl_list_init(&xserver->xi_selects);
	wl_list_init(&xserver->shape_selects);
	wl_list_init(&xserver->damages);
	wl_list_init(&xserver->redirects);
	wl_list_init(&xserver->overlay_refs);
	wl_list_init(&xserver->glx_objects);
	xserver->monitor_ids = 0x200;
	xserver->monitors_time = xs_now(xserver);
	xserver->last_input = xserver->monitors_time;
	xserver->trace = getenv("FLAME_X_TRACE") != NULL;
	if (!xs_atoms_init(&xserver->atoms)) {
		return false;
	}
	xserver->root = xs_window_create(xserver, NULL, XS_ROOT, NULL);
	if (!xserver->root) {
		xs_atoms_finish(&xserver->atoms);
		return false;
	}
	xserver->root->width = (uint16_t)(width > 0 ? width : 1920);
	xserver->root->height = (uint16_t)(height > 0 ? height : 1080);
	xserver->root->mapped = true;
	xserver->focus = XS_FOCUS_POINTER_ROOT;
	struct xs_window *flames = xs_window_create(xserver, NULL, XS_FLAME_WINDOW, xserver->root);
	if (flames) {
		flames->class = 2; // InputOnly
	}
	if (!flames || !xs_xkb_init(xserver)) {
		xs_window_destroy(xserver, xserver->root);
		xs_atoms_finish(&xserver->atoms);
		return false;
	}
	return true;
}

// a lock file we made, or one a dead server left behind
static bool take_lock(const char *path) {
	for (int attempt = 0; attempt < 2; attempt++) {
		int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0444);
		if (fd >= 0) {
			char pid[16];
			int len = snprintf(pid, sizeof(pid), "%10d\n", (int)getpid());
			bool ok = write(fd, pid, (size_t)len) == len;
			close(fd);
			if (!ok) {
				unlink(path);
			}
			return ok;
		}
		if (errno != EEXIST) {
			return false;
		}
		char buf[16] = {0};
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0) {
			return false;
		}
		ssize_t n = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		long owner = n > 0 ? strtol(buf, NULL, 10) : 0;
		if (owner <= 0 || (kill((pid_t)owner, 0) < 0 && errno == ESRCH)) {
			unlink(path); // stale, try again
			continue;
		}
		return false;
	}
	return false;
}

static int listen_on(const struct sockaddr_un *addr, socklen_t len) {
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0) {
		return -1;
	}
	if (bind(fd, (const struct sockaddr *)addr, len) < 0 || listen(fd, 64) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

bool xserver_start(struct flame_xserver *xserver) {
	mkdir("/tmp/.X11-unix", 01777);
	// the first free one, like xwayland: the lock files say who has which.
	// apps take :10 and up for ssh forwarding (mpv won't share memory with
	// those), so the low ones it is
	for (int display = 0; display < 128; display++) {
		snprintf(xserver->lock_path, sizeof(xserver->lock_path), "/tmp/.X%d-lock", display);
		if (!take_lock(xserver->lock_path)) {
			continue;
		}
		snprintf(xserver->socket_path, sizeof(xserver->socket_path),
			"/tmp/.X11-unix/X%d", display);
		unlink(xserver->socket_path); // ours now, by the lock

		struct sockaddr_un addr = {.sun_family = AF_UNIX};
		snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", xserver->socket_path);
		xserver->fds[0] = listen_on(&addr, sizeof(addr));
		// xlib tries the abstract name first, inside sandboxes too
		struct sockaddr_un abstract = {.sun_family = AF_UNIX};
		int len = snprintf(abstract.sun_path + 1, sizeof(abstract.sun_path) - 1,
			"%s", xserver->socket_path);
		xserver->fds[1] = listen_on(&abstract,
			(socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + (size_t)len));
		if (xserver->fds[0] < 0) {
			if (xserver->fds[1] >= 0) {
				close(xserver->fds[1]);
				xserver->fds[1] = -1;
			}
			unlink(xserver->lock_path);
			continue;
		}
		for (int i = 0; i < 2; i++) {
			if (xserver->fds[i] >= 0) {
				xserver->sources[i] = wl_event_loop_add_fd(xserver->loop, xserver->fds[i],
					WL_EVENT_READABLE, handle_accept, xserver);
			}
		}
		xserver->display = display;
		wlr_log(WLR_INFO, "xserver: listening on :%d", display);
		return true;
	}
	wlr_log(WLR_ERROR, "xserver: no free display number");
	return false;
}

void xserver_finish(struct flame_xserver *xserver) {
	// flame let go of its side already, owners going away below tell nobody
	xserver->hooks.selection_owner = NULL;
	xserver->hooks.selection_request = NULL;
	struct xs_client *client, *tmp;
	wl_list_for_each_safe(client, tmp, &xserver->clients, link) {
		client_free(client);
	}
	for (int i = 0; i < 2; i++) {
		if (xserver->sources[i]) {
			wl_event_source_remove(xserver->sources[i]);
			xserver->sources[i] = NULL;
		}
		if (xserver->fds[i] >= 0) {
			close(xserver->fds[i]);
			xserver->fds[i] = -1;
		}
	}
	if (xserver->display >= 0) {
		unlink(xserver->socket_path);
		unlink(xserver->lock_path);
		xserver->display = -1;
	}
	if (xserver->root) {
		xs_window_destroy(xserver, xserver->root);
		xserver->root = NULL;
	}
	// last: tearing windows down above queues one more
	if (xserver->changed_idle) {
		wl_event_source_remove(xserver->changed_idle);
		xserver->changed_idle = NULL;
	}
	if (xserver->changed_timer) {
		wl_event_source_remove(xserver->changed_timer);
		xserver->changed_timer = NULL;
	}
	if (xserver->damage_idle) {
		wl_event_source_remove(xserver->damage_idle);
		xserver->damage_idle = NULL;
	}
	xs_render_finish(xserver);
	xs_randr_client_gone(xserver, NULL);
	xs_xi_client_gone(xserver, NULL);
	xs_shape_client_gone(xserver, NULL);
	xs_damage_client_gone(xserver, NULL);
	xs_font_client_gone(xserver, NULL);
	xs_cursor_client_gone(xserver, NULL);
	xs_composite_client_gone(xserver, NULL);
	xs_glx_client_gone(xserver, NULL);
	xs_xkb_finish(xserver);
	xs_selection_finish(xserver);
	free(xserver->monitors);
	xserver->monitors = NULL;
	xserver->monitors_len = 0;
	xs_atoms_finish(&xserver->atoms);
}

void xserver_set_monitors(struct flame_xserver *xserver, const struct xs_monitor *monitors,
		size_t len) {
	struct xs_monitor *copy = len ? calloc(len, sizeof(*copy)) : NULL;
	if (len && !copy) {
		return;
	}
	int width = 0, height = 0;
	for (size_t i = 0; i < len; i++) {
		copy[i] = monitors[i];
		copy[i].name[sizeof(copy[i].name) - 1] = '\0';
		copy[i].id = 0;
		for (size_t j = 0; j < xserver->monitors_len; j++) {
			if (!strcmp(xserver->monitors[j].name, copy[i].name)) {
				copy[i].id = xserver->monitors[j].id;
			}
		}
		if (!copy[i].id) {
			copy[i].id = xserver->monitor_ids;
			xserver->monitor_ids += 4;
		}
		width = copy[i].x + copy[i].width > width ? copy[i].x + copy[i].width : width;
		height = copy[i].y + copy[i].height > height ? copy[i].y + copy[i].height : height;
	}
	bool same = len == xserver->monitors_len
		&& (!len || !memcmp(copy, xserver->monitors, len * sizeof(*copy)));
	if (same) {
		free(copy);
		return;
	}
	free(xserver->monitors);
	xserver->monitors = copy;
	xserver->monitors_len = len;
	xserver->monitors_time = xs_now(xserver);
	struct xs_window *root = xserver->root;
	if (width > 0 && height > 0 && (root->width != width || root->height != height)) {
		xs_window_configure(xserver, root, 4 | 8, 0, 0, width, height, 0, 0, 0);
	}
	xs_randr_changed(xserver);
}

void xserver_activity(struct flame_xserver *xserver) {
	xserver->last_input = xs_now(xserver);
}

uint32_t xserver_atom(struct flame_xserver *xserver, const char *name) {
	return xs_atom_intern(&xserver->atoms, name, strlen(name), true);
}

const char *xserver_atom_name(struct flame_xserver *xserver, uint32_t atom) {
	return xs_atom_name(&xserver->atoms, atom);
}

struct xs_window *xserver_window(struct flame_xserver *xserver, uint32_t id) {
	struct xs_window *window;
	wl_list_for_each(window, &xserver->windows, all_link) {
		if (window->id == id) {
			return window;
		}
	}
	return NULL;
}

bool xs_window_top_level(const struct xs_window *window) {
	return window->parent && !window->parent->parent;
}

bool xs_window_viewable(const struct xs_window *window) {
	for (; window; window = window->parent) {
		if (!window->mapped) {
			return false;
		}
	}
	return true;
}

struct xs_property *xserver_property(struct flame_xserver *xserver, struct xs_window *window,
		const char *name) {
	uint32_t atom = xs_atom_intern(&xserver->atoms, name, strlen(name), false);
	return atom ? xs_window_property(window, atom) : NULL;
}

const char *xs_window_title(struct flame_xserver *xserver, struct xs_window *window,
		char *buf, size_t size) {
	uint32_t net_wm_name = xs_atom_intern(&xserver->atoms, "_NET_WM_NAME", 12, false);
	struct xs_property *property = net_wm_name ? xs_window_property(window, net_wm_name) : NULL;
	if (!property || property->format != 8) {
		property = xs_window_property(window, XS_ATOM_WM_NAME);
	}
	if (!property || property->format != 8 || property->units == 0 || size == 0) {
		return NULL;
	}
	size_t len = property->units < size - 1 ? property->units : size - 1;
	memcpy(buf, property->data, len);
	buf[len] = '\0';
	// titles go into one line of ipc json and bars, no newlines
	for (size_t i = 0; i < len; i++) {
		if (buf[i] == '\n' || buf[i] == '\r') {
			buf[i] = ' ';
		}
	}
	return buf;
}

void xserver_move_window(struct flame_xserver *xserver, struct xs_window *window,
		int x, int y, int width, int height) {
	if (window->x == x && window->y == y && window->width == width
			&& window->height == height) {
		return;
	}
	xs_window_configure(xserver, window, 1 | 2 | 4 | 8, x, y, width, height, 0, 0, 0);
}

void xserver_set_focus(struct flame_xserver *xserver, uint32_t focus) {
	if (focus != xserver->focus) {
		xs_focus_set(xserver, focus, 2); // revert to parent
	}
}
