#include <stdlib.h>
#include <string.h>

#include "xserver/internal.h"

// Present: an app hands over a pixmap (a gpu buffer from DRI3) to show in a
// window, flame shows it and says when it hit the screen (CompleteNotify,
// with the kernel's flip timestamp) and when the buffer is free again
// (IdleNotify, or the release point with explicit sync). this is the path X11
// games and vulkan apps draw through, its timing is flame's frame scheduler

#define PRESENT_MINOR 4

#define EVENT_CONFIGURE 0
#define EVENT_COMPLETE 1
#define EVENT_IDLE 2

#define MASK_CONFIGURE 1
#define MASK_COMPLETE 2
#define MASK_IDLE 4

#define CAP_ASYNC 1
#define CAP_SYNCOBJ 16

static struct xs_pixmap *find_pixmap(struct flame_xserver *xserver, uint32_t id) {
	struct xs_pixmap *pixmap;
	wl_list_for_each(pixmap, &xserver->pixmaps, link) {
		if (pixmap->id == id) {
			return pixmap;
		}
	}
	return NULL;
}

static struct xs_syncobj *find_syncobj(struct flame_xserver *xserver, uint32_t id) {
	struct xs_syncobj *syncobj;
	wl_list_for_each(syncobj, &xserver->syncobjs, link) {
		if (syncobj->id == id) {
			return syncobj;
		}
	}
	return NULL;
}

// a generic event (XGE) to everyone who selected mask on window. len is 32 or
// more, the sequence number gets stamped per recipient
static void send_generic(struct flame_xserver *xserver, uint32_t window, uint32_t mask,
		uint8_t *event, size_t len) {
	struct xs_present_select *select;
	wl_list_for_each(select, &xserver->present_selects, link) {
		if (select->window != window || !(select->mask & mask)) {
			continue;
		}
		// the selection's own id goes in every Present event at 12
		memcpy(event + 12, &select->id, 4);
		memcpy(event + 2, &select->client->seq, 2);
		xs_client_send(select->client, event, len);
	}
}

static uint8_t *generic_header(uint8_t *event, uint16_t type, size_t len) {
	memset(event, 0, len);
	event[0] = 35; // GenericEvent
	event[1] = XS_PRESENT_OPCODE;
	uint32_t extra = (uint32_t)((len - 32) / 4);
	memcpy(event + 4, &extra, 4);
	memcpy(event + 8, &type, 2);
	return event;
}

void xserver_present_complete(struct flame_xserver *xserver, uint32_t window,
		uint32_t serial, bool notify_msc, uint8_t mode, uint64_t ust, uint64_t msc) {
	uint8_t event[40];
	generic_header(event, EVENT_COMPLETE, sizeof(event));
	event[10] = notify_msc ? 1 : 0; // kind
	event[11] = mode;
	memcpy(event + 16, &window, 4);
	memcpy(event + 20, &serial, 4);
	memcpy(event + 24, &ust, 8);
	memcpy(event + 32, &msc, 8);
	send_generic(xserver, window, MASK_COMPLETE, event, sizeof(event));
}

void xserver_present_idle(struct flame_xserver *xserver, uint32_t window, uint32_t serial,
		uint32_t pixmap, uint32_t idle_fence) {
	if (idle_fence) {
		xs_fence_trigger(xserver, idle_fence);
	}
	uint8_t event[32];
	generic_header(event, EVENT_IDLE, sizeof(event));
	memcpy(event + 16, &window, 4);
	memcpy(event + 20, &serial, 4);
	memcpy(event + 24, &pixmap, 4);
	memcpy(event + 28, &idle_fence, 4);
	send_generic(xserver, window, MASK_IDLE, event, sizeof(event));
}

void xs_present_configure(struct flame_xserver *xserver, struct xs_window *window) {
	uint8_t event[40];
	generic_header(event, EVENT_CONFIGURE, sizeof(event));
	int16_t x = window->x, y = window->y;
	memcpy(event + 16, &window->id, 4);
	memcpy(event + 20, &x, 2);
	memcpy(event + 22, &y, 2);
	memcpy(event + 24, &window->width, 2);
	memcpy(event + 26, &window->height, 2);
	// off_x/off_y stay 0, then the size a pixmap needs to fill the window
	memcpy(event + 32, &window->width, 2);
	memcpy(event + 34, &window->height, 2);
	send_generic(xserver, window->id, MASK_CONFIGURE, event, sizeof(event));
}

static void select_input(struct xs_client *client, uint32_t id, uint32_t window,
		uint32_t mask) {
	struct flame_xserver *xserver = client->xserver;
	struct xs_present_select *select, *tmp;
	wl_list_for_each_safe(select, tmp, &xserver->present_selects, link) {
		if (select->id == id && select->client == client) {
			if (!mask) {
				wl_list_remove(&select->link);
				free(select);
			} else {
				select->mask = mask;
			}
			return;
		}
	}
	if (!mask) {
		return;
	}
	if (!xs_id_available(client, id)) {
		xs_error(client, XS_ERROR_ID_CHOICE, id, XS_PRESENT_OPCODE, 3);
		return;
	}
	select = calloc(1, sizeof(*select));
	if (!select) {
		xs_error(client, XS_ERROR_ALLOC, id, XS_PRESENT_OPCODE, 3);
		return;
	}
	select->id = id;
	select->client = client;
	select->window = window;
	select->mask = mask;
	wl_list_insert(&xserver->present_selects, &select->link);
}

// Pixmap and PixmapSynced share their start, synced puts syncobjs where the
// fences were and moves everything after
static void present_pixmap(struct xs_client *client, const struct xin *in, bool synced) {
	struct flame_xserver *xserver = client->xserver;
	uint8_t minor = synced ? 5 : 1;
	uint32_t window_id = xin32(in, 4), pixmap_id = xin32(in, 8);
	struct xs_window *window = xserver_window(xserver, window_id);
	if (!window) {
		xs_error(client, XS_ERROR_WINDOW, window_id, XS_PRESENT_OPCODE, minor);
		return;
	}
	struct xs_pixmap *pixmap = find_pixmap(xserver, pixmap_id);
	if (!pixmap) {
		xs_error(client, XS_ERROR_PIXMAP, pixmap_id, XS_PRESENT_OPCODE, minor);
		return;
	}
	struct xs_present present = {
		.client = client,
		.window = window_id,
		.pixmap = pixmap,
		.serial = xin32(in, 12),
		.x_off = xin16s(in, 24),
		.y_off = xin16s(in, 26),
	};
	if (synced) {
		present.acquire = find_syncobj(xserver, xin32(in, 32));
		present.release = find_syncobj(xserver, xin32(in, 36));
		present.acquire_point = xin64(in, 40);
		present.release_point = xin64(in, 48);
		present.options = xin32(in, 56);
		present.target_msc = xin64(in, 64);
		present.divisor = xin64(in, 72);
		present.remainder = xin64(in, 80);
		if (!present.acquire || !present.release) {
			xs_error(client, XS_ERROR_VALUE, 0, XS_PRESENT_OPCODE, minor);
			return;
		}
	} else {
		present.idle_fence = xin32(in, 36);
		present.options = xin32(in, 40);
		present.target_msc = xin64(in, 48);
		present.divisor = xin64(in, 56);
		present.remainder = xin64(in, 64);
	}
	if (xserver->hooks.present) {
		xserver->hooks.present(xserver->hooks.data, &present);
	}
	// drawn on the gpu, it's all new as far as anyone can tell
	struct xs_window *shown = xserver_window(xserver, present.window);
	if (shown) {
		xs_damage_window_whole(xserver, shown);
	}
}

void xs_present_request(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	struct xout out;
	uint8_t minor = xin8(in, 1);
	switch (minor) {
	case 0: { // QueryVersion: 1.4, or the client's below that
		uint32_t major = xin32(in, 4), client_minor = xin32(in, 8);
		xs_reply_begin(client, &out, 0);
		xout32(&out, 1);
		xout32(&out, major > 1 || client_minor > PRESENT_MINOR ? PRESENT_MINOR : client_minor);
		xs_reply_send(client, &out);
		break;
	}
	case 1:
		present_pixmap(client, in, false);
		break;
	case 2: // NotifyMSC
		if (xserver->hooks.notify_msc) {
			xserver->hooks.notify_msc(xserver->hooks.data, client, xin32(in, 4), xin32(in, 8),
				xin64(in, 16), xin64(in, 24), xin64(in, 32));
		}
		break;
	case 3: // SelectInput
		select_input(client, xin32(in, 4), xin32(in, 8), xin32(in, 12));
		break;
	case 4: // QueryCapabilities: tearing on request, and explicit sync
		xs_reply_begin(client, &out, 0);
		xout32(&out, CAP_ASYNC | CAP_SYNCOBJ);
		xs_reply_send(client, &out);
		break;
	case 5:
		present_pixmap(client, in, true);
		break;
	default:
		xs_error(client, XS_ERROR_REQUEST, 0, XS_PRESENT_OPCODE, minor);
		break;
	}
}

void xs_present_window_gone(struct flame_xserver *xserver, uint32_t window) {
	struct xs_present_select *select, *tmp;
	wl_list_for_each_safe(select, tmp, &xserver->present_selects, link) {
		if (select->window == window) {
			wl_list_remove(&select->link);
			free(select);
		}
	}
}
