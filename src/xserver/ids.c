#include <stdlib.h>
#include <string.h>

#include "xserver/internal.h"

// resource ids: one namespace per client, whatever the resource. a client
// makes them up from the range it was handed, XC-MISC finds it more when it
// ran through them all (long running apps do: xlib asks on its own)

#define MASK 0x1fffffu

bool xs_id_in_use(struct flame_xserver *xserver, uint32_t id) {
	if (xserver_window(xserver, id) || xs_pixmap_find(xserver, id)
			|| xs_render_id_in_use(xserver, id)) {
		return true;
	}
	struct xs_gc *gc;
	wl_list_for_each(gc, &xserver->gcs, link) {
		if (gc->id == id) {
			return true;
		}
	}
	struct xs_fence *fence;
	wl_list_for_each(fence, &xserver->fences, link) {
		if (fence->id == id) {
			return true;
		}
	}
	struct xs_syncobj *syncobj;
	wl_list_for_each(syncobj, &xserver->syncobjs, link) {
		if (syncobj->id == id) {
			return true;
		}
	}
	struct xs_region *region;
	wl_list_for_each(region, &xserver->regions, link) {
		if (region->id == id) {
			return true;
		}
	}
	struct xs_shm_segment *segment;
	wl_list_for_each(segment, &xserver->shm_segments, link) {
		if (segment->id == id) {
			return true;
		}
	}
	struct xs_present_select *select;
	wl_list_for_each(select, &xserver->present_selects, link) {
		if (select->id == id) {
			return true;
		}
	}
	return false;
}

bool xs_id_available(struct xs_client *client, uint32_t id) {
	return (id & ~MASK) == client->id_base && !xs_id_in_use(client->xserver, id);
}

// ---- XC-MISC ----

static int compare(const void *a, const void *b) {
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
	return x < y ? -1 : x > y;
}

static void add(uint32_t **ids, size_t *len, size_t *cap, uint32_t base, uint32_t id) {
	if ((id & ~MASK) != base) {
		return;
	}
	if (*len == *cap) {
		size_t grown = *cap ? *cap * 2 : 256;
		uint32_t *more = realloc(*ids, grown * sizeof(**ids));
		if (!more) {
			return;
		}
		*ids = more;
		*cap = grown;
	}
	(*ids)[(*len)++] = id & MASK;
}

// the low bits of every id the client has in use, sorted. NULL: none
static uint32_t *used_ids(struct xs_client *client, size_t *len) {
	struct flame_xserver *xserver = client->xserver;
	uint32_t base = client->id_base, *ids = NULL;
	size_t cap = 0;
	*len = 0;
	struct xs_window *window;
	wl_list_for_each(window, &xserver->windows, all_link) {
		add(&ids, len, &cap, base, window->id);
	}
	struct xs_pixmap *pixmap;
	wl_list_for_each(pixmap, &xserver->pixmaps, link) {
		add(&ids, len, &cap, base, pixmap->id);
	}
	struct xs_gc *gc;
	wl_list_for_each(gc, &xserver->gcs, link) {
		add(&ids, len, &cap, base, gc->id);
	}
	struct xs_fence *fence;
	wl_list_for_each(fence, &xserver->fences, link) {
		add(&ids, len, &cap, base, fence->id);
	}
	struct xs_syncobj *syncobj;
	wl_list_for_each(syncobj, &xserver->syncobjs, link) {
		add(&ids, len, &cap, base, syncobj->id);
	}
	struct xs_region *region;
	wl_list_for_each(region, &xserver->regions, link) {
		add(&ids, len, &cap, base, region->id);
	}
	struct xs_shm_segment *segment;
	wl_list_for_each(segment, &xserver->shm_segments, link) {
		add(&ids, len, &cap, base, segment->id);
	}
	struct xs_present_select *select;
	wl_list_for_each(select, &xserver->present_selects, link) {
		add(&ids, len, &cap, base, select->id);
	}
	// render's own lists, as many as there are
	size_t n = xs_render_ids(xserver, base, NULL, 0);
	uint32_t *render = n ? malloc(n * sizeof(*render)) : NULL;
	if (render) {
		n = xs_render_ids(xserver, base, render, n);
		for (size_t i = 0; i < n; i++) {
			add(&ids, len, &cap, base, render[i]);
		}
		free(render);
	}
	if (ids) {
		qsort(ids, *len, sizeof(*ids), compare);
	}
	return ids;
}

// the longest run of ids nobody uses
static void get_xid_range(struct xs_client *client) {
	size_t len;
	uint32_t *ids = used_ids(client, &len);
	uint32_t best_start = 0, best_count = 0, next = 1; // low bits 0 is the base itself
	for (size_t i = 0; i <= len; i++) {
		uint32_t end = i < len ? ids[i] : MASK + 1;
		if (end > next && end - next > best_count) {
			best_start = next;
			best_count = end - next;
		}
		if (i < len && ids[i] + 1 > next) {
			next = ids[i] + 1;
		}
	}
	free(ids);
	struct xout out;
	xs_reply_begin(client, &out, 0);
	xout32(&out, best_count ? client->id_base | best_start : 0);
	xout32(&out, best_count);
	xs_reply_send(client, &out);
}

static void get_xid_list(struct xs_client *client, const struct xin *in) {
	uint32_t want = xin32(in, 4);
	want = want > 65536 ? 65536 : want;
	size_t len;
	uint32_t *ids = used_ids(client, &len);
	struct xout out;
	xs_reply_begin(client, &out, 0);
	size_t count_at = out.buf.len;
	xout32(&out, 0);
	xout_zero(&out, 20);
	uint32_t count = 0;
	size_t i = 0;
	for (uint32_t low = 1; low <= MASK && count < want; low++) {
		while (i < len && ids[i] < low) {
			i++;
		}
		if (i < len && ids[i] == low) {
			continue;
		}
		xout32(&out, client->id_base | low);
		count++;
	}
	free(ids);
	if (!out.buf.failed) {
		memcpy(out.buf.data + count_at, &count, 4);
	}
	xs_reply_send(client, &out);
}

void xs_xcmisc_request(struct xs_client *client, const struct xin *in) {
	struct xout out;
	uint8_t minor = xin8(in, 1);
	switch (minor) {
	case 0: // GetVersion: 1.1
		xs_reply_begin(client, &out, 0);
		xout16(&out, 1);
		xout16(&out, 1);
		xs_reply_send(client, &out);
		break;
	case 1:
		get_xid_range(client);
		break;
	case 2:
		get_xid_list(client, in);
		break;
	default:
		xs_error(client, XS_ERROR_REQUEST, 0, XS_XCMISC_OPCODE, minor);
		break;
	}
}
