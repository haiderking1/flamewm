#include <stdlib.h>

#include "xserver/internal.h"

// DAMAGE 1.1: clients hear which pixels of a window or pixmap changed. VNC
// servers, screen recorders, Wine's and toolkits' redirected windows. every
// drawing request ends in the region it touched, that region lands here.
// a window's damage includes what its children draw, the root's everything
// on screen

#define BAD_DAMAGE (XS_DAMAGE_FIRST_ERROR + 0)

enum {
	LEVEL_RAW,
	LEVEL_DELTA,
	LEVEL_BOX,
	LEVEL_NONEMPTY,
};

struct damage {
	struct wl_list link; // flame_xserver.damages
	uint32_t id;
	struct xs_client *owner;
	uint32_t drawable;
	uint8_t level;
	pixman_region32_t region; // drawable coords, what's damaged now
};

static struct damage *find(struct flame_xserver *xserver, uint32_t id) {
	struct damage *d;
	wl_list_for_each(d, &xserver->damages, link) {
		if (d->id == id) {
			return d;
		}
	}
	return NULL;
}

static void damage_free(struct damage *d) {
	wl_list_remove(&d->link);
	pixman_region32_fini(&d->region);
	free(d);
}

// where the drawable is and how big: windows by their inside corner, root
// coords. false: it's gone
static bool geometry(struct flame_xserver *xserver, uint32_t id, int *x, int *y, int *w,
		int *h) {
	struct xs_window *window = xserver_window(xserver, id);
	if (window) {
		xs_window_origin(window, x, y);
		*w = window->width;
		*h = window->height;
		return true;
	}
	struct xs_pixmap *pixmap = xs_pixmap_find(xserver, id);
	if (pixmap) {
		*x = *y = 0;
		*w = pixmap->width;
		*h = pixmap->height;
		return true;
	}
	return false;
}

// DamageNotify for each box, all but the last say more are coming. no
// boxes: one for the whole drawable
static void notify(struct flame_xserver *xserver, struct damage *d,
		const pixman_box32_t *boxes, int n) {
	int gx, gy, gw, gh;
	if (!geometry(xserver, d->drawable, &gx, &gy, &gw, &gh)) {
		return;
	}
	pixman_box32_t whole = {0, 0, gw, gh};
	if (!n) {
		boxes = &whole;
		n = 1;
	}
	for (int i = 0; i < n; i++) {
		struct xs_event event = {0};
		xs_event_put8(&event, 0, XS_DAMAGE_FIRST_EVENT);
		xs_event_put8(&event, 1, (uint8_t)(d->level | (i + 1 < n ? 0x80 : 0)));
		xs_event_put32(&event, 4, d->drawable);
		xs_event_put32(&event, 8, d->id);
		xs_event_put32(&event, 12, xs_now(xserver));
		xs_event_put16(&event, 16, (uint16_t)boxes[i].x1);
		xs_event_put16(&event, 18, (uint16_t)boxes[i].y1);
		xs_event_put16(&event, 20, (uint16_t)(boxes[i].x2 - boxes[i].x1));
		xs_event_put16(&event, 22, (uint16_t)(boxes[i].y2 - boxes[i].y1));
		xs_event_put16(&event, 24, (uint16_t)gx);
		xs_event_put16(&event, 26, (uint16_t)gy);
		xs_event_put16(&event, 28, (uint16_t)gw);
		xs_event_put16(&event, 30, (uint16_t)gh);
		xs_event_send(d->owner, &event);
	}
}

static void report_region(struct flame_xserver *xserver, struct damage *d,
		const pixman_region32_t *region) {
	int n;
	const pixman_box32_t *boxes = pixman_region32_rectangles((pixman_region32_t *)region, &n);
	if (n) {
		notify(xserver, d, boxes, n);
	}
}

// added (drawable coords, already inside it) got drawn: d keeps it and its
// owner hears as much as its level asks for
static void add(struct flame_xserver *xserver, struct damage *d,
		const pixman_region32_t *added) {
	if (!pixman_region32_not_empty(added)) {
		return;
	}
	switch (d->level) {
	case LEVEL_RAW:
		// nothing is kept: Subtract leaves raw ones alone, nobody could read it
		report_region(xserver, d, added);
		break;
	case LEVEL_DELTA: {
		pixman_region32_t fresh;
		pixman_region32_init(&fresh);
		pixman_region32_subtract(&fresh, added, &d->region);
		pixman_region32_union(&d->region, &d->region, added);
		report_region(xserver, d, &fresh);
		pixman_region32_fini(&fresh);
		break;
	}
	case LEVEL_BOX: {
		pixman_box32_t was = *pixman_region32_extents(&d->region);
		bool was_empty = !pixman_region32_not_empty(&d->region);
		pixman_region32_union(&d->region, &d->region, added);
		pixman_box32_t now = *pixman_region32_extents(&d->region);
		if (was_empty || now.x1 < was.x1 || now.y1 < was.y1 || now.x2 > was.x2
				|| now.y2 > was.y2) {
			notify(xserver, d, &now, 1);
		}
		break;
	}
	default: { // non-empty
		bool was_empty = !pixman_region32_not_empty(&d->region);
		pixman_region32_union(&d->region, &d->region, added);
		if (was_empty) {
			notify(xserver, d, NULL, 0);
		}
		break;
	}
	}
}

// region (drawable coords) drawn on drawable: everyone tracking it hears,
// cut to the drawable
static void drawable_damaged(struct flame_xserver *xserver, uint32_t drawable,
		const pixman_region32_t *region) {
	if (wl_list_empty(&xserver->damages)) {
		return;
	}
	int x, y, w, h;
	if (!geometry(xserver, drawable, &x, &y, &w, &h)) {
		return;
	}
	pixman_region32_t clipped;
	pixman_region32_init(&clipped);
	pixman_region32_intersect_rect(&clipped, region, 0, 0, (unsigned)w, (unsigned)h);
	struct damage *d, *tmp;
	wl_list_for_each_safe(d, tmp, &xserver->damages, link) {
		if (d->drawable == drawable) {
			add(xserver, d, &clipped);
		}
	}
	pixman_region32_fini(&clipped);
}

void xs_damage_backing(struct flame_xserver *xserver, struct xs_window *top,
		const pixman_region32_t *region) {
	if (wl_list_empty(&xserver->damages) || !top) {
		return;
	}
	int tx, ty;
	xs_window_origin(top, &tx, &ty);
	pixman_region32_t r;
	pixman_region32_init(&r);
	struct damage *d, *tmp;
	wl_list_for_each_safe(d, tmp, &xserver->damages, link) {
		struct xs_window *window = xserver_window(xserver, d->drawable);
		if (!window) {
			continue;
		}
		// a window in this top-level (itself included), or the root
		int wx = 0, wy = 0;
		if (window->parent) {
			if (xs_window_top(window) != top || !xs_window_viewable(window)) {
				continue;
			}
			xs_window_origin(window, &wx, &wy);
		}
		pixman_region32_copy(&r, region);
		pixman_region32_translate(&r, tx - wx, ty - wy);
		pixman_region32_intersect_rect(&r, &r, 0, 0, window->width, window->height);
		add(xserver, d, &r);
	}
	pixman_region32_fini(&r);
}

void xs_damage_pixmap(struct flame_xserver *xserver, struct xs_pixmap *pixmap,
		const pixman_region32_t *region) {
	drawable_damaged(xserver, pixmap->id, region);
}

void xs_damage_window_whole(struct flame_xserver *xserver, struct xs_window *window) {
	if (wl_list_empty(&xserver->damages)) {
		return;
	}
	struct xs_window *top = xs_window_top(window);
	int tx, ty, wx, wy;
	if (!top) {
		return;
	}
	xs_window_origin(top, &tx, &ty);
	xs_window_origin(window, &wx, &wy);
	pixman_region32_t r;
	pixman_region32_init_rect(&r, wx - tx, wy - ty, window->width, window->height);
	xs_damage_backing(xserver, top, &r);
	pixman_region32_fini(&r);
}

void xs_damage_root(struct flame_xserver *xserver, int x, int y, int width, int height) {
	if (wl_list_empty(&xserver->damages) || width <= 0 || height <= 0) {
		return;
	}
	pixman_region32_t r;
	pixman_region32_init_rect(&r, x, y, (unsigned)width, (unsigned)height);
	drawable_damaged(xserver, xserver->root->id, &r);
	pixman_region32_fini(&r);
}

void xs_damage_drawable_gone(struct flame_xserver *xserver, uint32_t drawable) {
	struct damage *d, *tmp;
	wl_list_for_each_safe(d, tmp, &xserver->damages, link) {
		if (d->drawable == drawable) {
			damage_free(d);
		}
	}
}

void xs_damage_client_gone(struct flame_xserver *xserver, struct xs_client *client) {
	struct damage *d, *tmp;
	wl_list_for_each_safe(d, tmp, &xserver->damages, link) {
		if (!client || d->owner == client) {
			damage_free(d);
		}
	}
}

bool xs_damage_id_in_use(struct flame_xserver *xserver, uint32_t id) {
	return find(xserver, id) != NULL;
}

size_t xs_damage_ids(struct flame_xserver *xserver, uint32_t base, uint32_t *out, size_t max) {
	size_t len = 0;
	struct damage *d;
	wl_list_for_each(d, &xserver->damages, link) {
		if ((d->id & ~0x1fffffu) == base && (!out || len < max)) {
			if (out) {
				out[len] = d->id;
			}
			len++;
		}
	}
	return len;
}

// ---- requests ----

static void create(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	uint32_t id = xin32(in, 4), drawable = xin32(in, 8);
	uint8_t level = xin8(in, 12);
	int x, y, w, h;
	if (!geometry(xserver, drawable, &x, &y, &w, &h)) {
		xs_error(client, XS_ERROR_DRAWABLE, drawable, XS_DAMAGE_OPCODE, 1);
		return;
	}
	if (level > LEVEL_NONEMPTY) {
		xs_error(client, XS_ERROR_VALUE, level, XS_DAMAGE_OPCODE, 1);
		return;
	}
	if (!xs_id_available(client, id)) {
		xs_error(client, XS_ERROR_ID_CHOICE, id, XS_DAMAGE_OPCODE, 1);
		return;
	}
	struct damage *d = calloc(1, sizeof(*d));
	if (!d) {
		xs_error(client, XS_ERROR_ALLOC, id, XS_DAMAGE_OPCODE, 1);
		return;
	}
	d->id = id;
	d->owner = client;
	d->drawable = drawable;
	d->level = level;
	pixman_region32_init(&d->region);
	wl_list_insert(&xserver->damages, &d->link);
	// like Xorg: a window that shows starts out all damaged, the client
	// picks up what's there now
	struct xs_window *window = xserver_window(xserver, drawable);
	if (window && xs_window_viewable(window)) {
		pixman_region32_t whole;
		pixman_region32_init_rect(&whole, 0, 0, (unsigned)w, (unsigned)h);
		add(xserver, d, &whole);
		pixman_region32_fini(&whole);
	}
}

static struct damage *get(struct xs_client *client, uint32_t id, uint8_t minor) {
	struct damage *d = find(client->xserver, id);
	if (!d) {
		xs_error(client, BAD_DAMAGE, id, XS_DAMAGE_OPCODE, minor);
	}
	return d;
}

// a region argument, None allowed
static bool region_arg(struct xs_client *client, uint32_t id, uint8_t minor,
		struct xs_region **out) {
	*out = id ? xs_region_find(client->xserver, id) : NULL;
	if (id && !*out) {
		xs_error(client, XS_XFIXES_FIRST_ERROR + 0, id, XS_DAMAGE_OPCODE, minor);
		return false;
	}
	return true;
}

static void subtract(struct xs_client *client, const struct xin *in) {
	struct damage *d = get(client, xin32(in, 4), 3);
	struct xs_region *repair, *parts;
	if (!d || !region_arg(client, xin32(in, 8), 3, &repair)
			|| !region_arg(client, xin32(in, 12), 3, &parts)) {
		return;
	}
	if (d->level == LEVEL_RAW) {
		return; // nothing is kept for raw reports, as in Xorg
	}
	if (!repair) {
		if (parts) {
			pixman_region32_copy(&parts->region, &d->region);
		}
		pixman_region32_clear(&d->region);
		return;
	}
	if (parts) {
		pixman_region32_intersect(&parts->region, &d->region, &repair->region);
	}
	pixman_region32_subtract(&d->region, &d->region, &repair->region);
	// what's left is told again, so it isn't lost
	if (pixman_region32_not_empty(&d->region)) {
		pixman_region32_t left;
		pixman_region32_init(&left);
		pixman_region32_copy(&left, &d->region);
		pixman_region32_clear(&d->region);
		add(client->xserver, d, &left);
		pixman_region32_fini(&left);
	}
}

static void add_request(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	uint32_t drawable = xin32(in, 4);
	struct xs_region *region;
	int x, y, w, h;
	if (!geometry(xserver, drawable, &x, &y, &w, &h)) {
		xs_error(client, XS_ERROR_DRAWABLE, drawable, XS_DAMAGE_OPCODE, 4);
		return;
	}
	if (!region_arg(client, xin32(in, 8), 4, &region) || !region) {
		if (!xin32(in, 8)) {
			xs_error(client, XS_XFIXES_FIRST_ERROR + 0, 0, XS_DAMAGE_OPCODE, 4);
		}
		return;
	}
	struct xs_window *window = xserver_window(xserver, drawable);
	struct xs_window *top = window ? xs_window_top(window) : NULL;
	if (top) {
		// as if it were drawn: the window and everything around it hears
		int tx, ty, wx, wy;
		xs_window_origin(top, &tx, &ty);
		xs_window_origin(window, &wx, &wy);
		pixman_region32_t r;
		pixman_region32_init(&r);
		pixman_region32_intersect_rect(&r, &region->region, 0, 0, window->width,
			window->height);
		pixman_region32_translate(&r, wx - tx, wy - ty);
		xs_damage_backing(xserver, top, &r);
		pixman_region32_fini(&r);
	} else {
		drawable_damaged(xserver, drawable, &region->region);
	}
}

void xs_damage_request(struct xs_client *client, const struct xin *in) {
	struct xout out;
	struct damage *d;
	uint8_t minor = xin8(in, 1);
	switch (minor) {
	case 0: // QueryVersion: 1.1
		xs_reply_begin(client, &out, 0);
		xout32(&out, 1);
		xout32(&out, 1);
		xs_reply_send(client, &out);
		break;
	case 1:
		create(client, in);
		break;
	case 2: // Destroy
		if ((d = get(client, xin32(in, 4), 2))) {
			damage_free(d);
		}
		break;
	case 3:
		subtract(client, in);
		break;
	case 4:
		add_request(client, in);
		break;
	default:
		xs_error(client, XS_ERROR_REQUEST, 0, XS_DAMAGE_OPCODE, minor);
		break;
	}
}
