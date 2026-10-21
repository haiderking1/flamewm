#include <stdlib.h>

#include "xserver/internal.h"

// XFIXES 5: regions (what DAMAGE, Composite and Present talk in, clips for
// GCs and pictures, window shapes), selection owner events (selection.c),
// cursor names, barriers. flame draws the pointer, cursors are just told
// back empty

#define BAD_REGION (XS_XFIXES_FIRST_ERROR + 0)

// ---- regions ----

struct xs_region *xs_region_find(struct flame_xserver *xserver, uint32_t id) {
	struct xs_region *region;
	wl_list_for_each(region, &xserver->regions, link) {
		if (region->id == id) {
			return region;
		}
	}
	return NULL;
}

void xs_region_free(struct xs_region *region) {
	wl_list_remove(&region->link);
	pixman_region32_fini(&region->region);
	free(region);
}

static struct xs_region *get_region(struct xs_client *client, uint32_t id, uint8_t minor) {
	struct xs_region *region = xs_region_find(client->xserver, id);
	if (!region) {
		xs_error(client, BAD_REGION, id, XS_XFIXES_OPCODE, minor);
	}
	return region;
}

// a new region holding what's in from (moved into it)
static void region_new(struct xs_client *client, uint32_t id, pixman_region32_t *from,
		uint8_t minor) {
	if (!xs_id_available(client, id)) {
		pixman_region32_fini(from);
		xs_error(client, XS_ERROR_ID_CHOICE, id, XS_XFIXES_OPCODE, minor);
		return;
	}
	struct xs_region *region = calloc(1, sizeof(*region));
	if (!region) {
		pixman_region32_fini(from);
		xs_error(client, XS_ERROR_ALLOC, id, XS_XFIXES_OPCODE, minor);
		return;
	}
	region->id = id;
	region->owner = client;
	pixman_region32_init(&region->region);
	pixman_region32_copy(&region->region, from);
	pixman_region32_fini(from);
	wl_list_insert(&client->xserver->regions, &region->link);
}

// RECTANGLE list from at to the end of the request
static void rectangles(const struct xin *in, size_t at, pixman_region32_t *out) {
	size_t n = in->len > at ? (in->len - at) / 8 : 0;
	pixman_region32_init(out);
	pixman_box32_t *boxes = n ? calloc(n, sizeof(*boxes)) : NULL;
	size_t used = 0;
	for (size_t i = 0; boxes && i < n; i++) {
		int x = xin16s(in, at + i * 8), y = xin16s(in, at + 2 + i * 8);
		int w = xin16(in, at + 4 + i * 8), h = xin16(in, at + 6 + i * 8);
		if (w && h) {
			boxes[used++] = (pixman_box32_t){x, y, x + w, y + h};
		}
	}
	if (used) {
		pixman_region32_fini(out);
		pixman_region32_init_rects(out, boxes, (int)used);
	}
	free(boxes);
}

static void create_from_bitmap(struct xs_client *client, const struct xin *in) {
	uint32_t id = xin32(in, 8);
	struct xs_pixmap *pixmap = xs_pixmap_find(client->xserver, id);
	if (!pixmap) {
		xs_error(client, XS_ERROR_PIXMAP, id, XS_XFIXES_OPCODE, 6);
		return;
	}
	if (pixmap->depth != 1 || !pixmap->image) {
		xs_error(client, XS_ERROR_MATCH, id, XS_XFIXES_OPCODE, 6);
		return;
	}
	pixman_region32_t region;
	pixman_region32_init_from_image(&region, pixmap->image);
	region_new(client, xin32(in, 4), &region, 6);
}

static void create_from_window(struct xs_client *client, const struct xin *in) {
	uint32_t id = xin32(in, 8);
	uint8_t kind = xin8(in, 12);
	struct xs_window *window = xserver_window(client->xserver, id);
	if (!window) {
		xs_error(client, XS_ERROR_WINDOW, id, XS_XFIXES_OPCODE, 7);
		return;
	}
	if (kind > 1) {
		xs_error(client, XS_ERROR_VALUE, kind, XS_XFIXES_OPCODE, 7);
		return;
	}
	pixman_region32_t region;
	xs_shape_get(window, kind, &region);
	region_new(client, xin32(in, 4), &region, 7);
}

static void create_from_gc(struct xs_client *client, const struct xin *in) {
	uint32_t id = xin32(in, 8);
	struct xs_gc *gc = xs_gc_find(client->xserver, id);
	if (!gc) {
		xs_error(client, XS_ERROR_GC, id, XS_XFIXES_OPCODE, 8);
		return;
	}
	if (!gc->clipped) {
		xs_error(client, XS_ERROR_MATCH, id, XS_XFIXES_OPCODE, 8);
		return;
	}
	pixman_region32_t region;
	pixman_region32_init(&region);
	pixman_region32_copy(&region, &gc->clip);
	region_new(client, xin32(in, 4), &region, 8);
}

static void create_from_picture(struct xs_client *client, const struct xin *in) {
	uint32_t id = xin32(in, 8);
	pixman_region32_t region;
	pixman_region32_init(&region);
	int error = xs_render_picture_clip(client->xserver, id, &region);
	if (error) {
		pixman_region32_fini(&region);
		xs_error(client, (uint8_t)error, id, XS_XFIXES_OPCODE, 9);
		return;
	}
	region_new(client, xin32(in, 4), &region, 9);
}

// Union, Intersect, Subtract: src1, src2 into dst
static void combine(struct xs_client *client, const struct xin *in, uint8_t minor) {
	struct xs_region *a = get_region(client, xin32(in, 4), minor);
	struct xs_region *b = a ? get_region(client, xin32(in, 8), minor) : NULL;
	struct xs_region *dst = b ? get_region(client, xin32(in, 12), minor) : NULL;
	if (!dst) {
		return;
	}
	if (minor == 13) {
		pixman_region32_union(&dst->region, &a->region, &b->region);
	} else if (minor == 14) {
		pixman_region32_intersect(&dst->region, &a->region, &b->region);
	} else {
		pixman_region32_subtract(&dst->region, &a->region, &b->region);
	}
}

static void invert(struct xs_client *client, const struct xin *in) {
	struct xs_region *src = get_region(client, xin32(in, 4), 16);
	struct xs_region *dst = src ? get_region(client, xin32(in, 16), 16) : NULL;
	if (!dst) {
		return;
	}
	pixman_box32_t bounds = {xin16s(in, 8), xin16s(in, 10), 0, 0};
	bounds.x2 = bounds.x1 + xin16(in, 12);
	bounds.y2 = bounds.y1 + xin16(in, 14);
	pixman_region32_inverse(&dst->region, &src->region, &bounds);
}

static void extents(struct xs_client *client, const struct xin *in) {
	struct xs_region *src = get_region(client, xin32(in, 4), 18);
	struct xs_region *dst = src ? get_region(client, xin32(in, 8), 18) : NULL;
	if (!dst) {
		return;
	}
	pixman_box32_t e = *pixman_region32_extents(&src->region);
	pixman_region32_fini(&dst->region);
	if (e.x2 > e.x1 && e.y2 > e.y1) {
		pixman_region32_init_rect(&dst->region, e.x1, e.y1, (unsigned)(e.x2 - e.x1),
			(unsigned)(e.y2 - e.y1));
	} else {
		pixman_region32_init(&dst->region);
	}
}

static void fetch(struct xs_client *client, const struct xin *in) {
	struct xs_region *region = get_region(client, xin32(in, 4), 19);
	if (!region) {
		return;
	}
	pixman_box32_t *e = pixman_region32_extents(&region->region);
	int n;
	pixman_box32_t *boxes = pixman_region32_rectangles(&region->region, &n);
	struct xout out;
	xs_reply_begin(client, &out, 0);
	xout16(&out, (uint16_t)e->x1);
	xout16(&out, (uint16_t)e->y1);
	xout16(&out, (uint16_t)(e->x2 - e->x1));
	xout16(&out, (uint16_t)(e->y2 - e->y1));
	xout_zero(&out, 16);
	for (int i = 0; i < n; i++) {
		xout16(&out, (uint16_t)boxes[i].x1);
		xout16(&out, (uint16_t)boxes[i].y1);
		xout16(&out, (uint16_t)(boxes[i].x2 - boxes[i].x1));
		xout16(&out, (uint16_t)(boxes[i].y2 - boxes[i].y1));
	}
	xs_reply_send(client, &out);
}

// every rectangle grown by left, right, top, bottom
static void expand(struct xs_client *client, const struct xin *in) {
	struct xs_region *src = get_region(client, xin32(in, 4), 28);
	struct xs_region *dst = src ? get_region(client, xin32(in, 8), 28) : NULL;
	if (!dst) {
		return;
	}
	int left = xin16(in, 12), right = xin16(in, 14), top = xin16(in, 16);
	int bottom = xin16(in, 18);
	int n;
	pixman_box32_t *boxes = pixman_region32_rectangles(&src->region, &n);
	pixman_region32_t grown;
	pixman_region32_init(&grown);
	for (int i = 0; i < n; i++) {
		pixman_region32_union_rect(&grown, &grown, boxes[i].x1 - left, boxes[i].y1 - top,
			(unsigned)(boxes[i].x2 - boxes[i].x1 + left + right),
			(unsigned)(boxes[i].y2 - boxes[i].y1 + top + bottom));
	}
	pixman_region32_copy(&dst->region, &grown);
	pixman_region32_fini(&grown);
}

// a region argument that may be None: false when it's neither
static bool region_or_none(struct xs_client *client, uint32_t id, uint8_t minor,
		struct xs_region **out) {
	*out = id ? get_region(client, id, minor) : NULL;
	return !id || *out;
}

static void set_gc_clip(struct xs_client *client, const struct xin *in) {
	uint32_t id = xin32(in, 4);
	struct xs_gc *gc = xs_gc_find(client->xserver, id);
	if (!gc) {
		xs_error(client, XS_ERROR_GC, id, XS_XFIXES_OPCODE, 20);
		return;
	}
	// gc, region, then the clip origin
	struct xs_region *region;
	if (!region_or_none(client, xin32(in, 8), 20, &region)) {
		return;
	}
	pixman_region32_clear(&gc->clip);
	gc->clipped = region != NULL;
	if (region) {
		pixman_region32_copy(&gc->clip, &region->region);
	}
	gc->clip_x = xin16s(in, 12);
	gc->clip_y = xin16s(in, 14);
}

static void set_window_shape(struct xs_client *client, const struct xin *in) {
	uint32_t id = xin32(in, 4);
	uint8_t kind = xin8(in, 8);
	struct xs_window *window = xserver_window(client->xserver, id);
	if (!window) {
		xs_error(client, XS_ERROR_WINDOW, id, XS_XFIXES_OPCODE, 21);
		return;
	}
	if (kind > 2) {
		xs_error(client, XS_ERROR_VALUE, kind, XS_XFIXES_OPCODE, 21);
		return;
	}
	struct xs_region *region;
	if (!region_or_none(client, xin32(in, 16), 21, &region)) {
		return;
	}
	if (!region) {
		xs_shape_set(client->xserver, window, kind, NULL);
		return;
	}
	pixman_region32_t shape;
	pixman_region32_init(&shape);
	pixman_region32_copy(&shape, &region->region);
	pixman_region32_translate(&shape, xin16s(in, 12), xin16s(in, 14));
	xs_shape_set(client->xserver, window, kind, &shape);
	pixman_region32_fini(&shape);
}

static void set_picture_clip(struct xs_client *client, const struct xin *in) {
	uint32_t id = xin32(in, 4);
	struct xs_region *region;
	if (!region_or_none(client, xin32(in, 8), 22, &region)) {
		return;
	}
	int error = xs_render_set_picture_clip(client->xserver, id,
		region ? &region->region : NULL, xin16s(in, 12), xin16s(in, 14));
	if (error) {
		xs_error(client, (uint8_t)error, id, XS_XFIXES_OPCODE, 22);
	}
}

// ---- cursors ----

static void get_cursor_image(struct xs_client *client, bool with_name) {
	// an empty one, flame draws the pointer
	struct xout out;
	xs_reply_begin(client, &out, 0);
	xout16(&out, 0); // x, y
	xout16(&out, 0);
	xout16(&out, 1); // width, height
	xout16(&out, 1);
	xout16(&out, 0); // hotspot
	xout16(&out, 0);
	xout32(&out, 1); // serial
	if (with_name) {
		xout32(&out, 0); // cursor atom
		xout16(&out, 0); // name length
		xout_zero(&out, 2);
	} else {
		xout_zero(&out, 8);
	}
	xout32(&out, 0); // the one pixel, transparent
	xs_reply_send(client, &out);
}

// ---- requests ----

void xs_xfixes_request(struct xs_client *client, const struct xin *in) {
	struct xout out;
	struct xs_region *region, *other;
	pixman_region32_t r;
	uint8_t minor = xin8(in, 1);
	switch (minor) {
	case 0: // QueryVersion
		xs_reply_begin(client, &out, 0);
		xout32(&out, 5);
		xout32(&out, 0);
		xs_reply_send(client, &out);
		break;
	case 2:
		xs_selection_select_input(client, in);
		break;
	case 4: // GetCursorImage
		get_cursor_image(client, false);
		break;
	case 5: // CreateRegion
		rectangles(in, 8, &r);
		region_new(client, xin32(in, 4), &r, 5);
		break;
	case 6:
		create_from_bitmap(client, in);
		break;
	case 7:
		create_from_window(client, in);
		break;
	case 8:
		create_from_gc(client, in);
		break;
	case 9:
		create_from_picture(client, in);
		break;
	case 10: // DestroyRegion
		if ((region = get_region(client, xin32(in, 4), 10))) {
			xs_region_free(region);
		}
		break;
	case 11: // SetRegion
		if ((region = get_region(client, xin32(in, 4), 11))) {
			rectangles(in, 8, &r);
			pixman_region32_copy(&region->region, &r);
			pixman_region32_fini(&r);
		}
		break;
	case 12: // CopyRegion
		if ((region = get_region(client, xin32(in, 4), 12))
				&& (other = get_region(client, xin32(in, 8), 12))) {
			pixman_region32_copy(&other->region, &region->region);
		}
		break;
	case 13: case 14: case 15:
		combine(client, in, minor);
		break;
	case 16:
		invert(client, in);
		break;
	case 17: // TranslateRegion
		if ((region = get_region(client, xin32(in, 4), 17))) {
			pixman_region32_translate(&region->region, xin16s(in, 8), xin16s(in, 10));
		}
		break;
	case 18:
		extents(client, in);
		break;
	case 19:
		fetch(client, in);
		break;
	case 20:
		set_gc_clip(client, in);
		break;
	case 21:
		set_window_shape(client, in);
		break;
	case 22:
		set_picture_clip(client, in);
		break;
	case 24: // GetCursorName: none
		xs_reply_begin(client, &out, 0);
		xout32(&out, 0);
		xout16(&out, 0);
		xout_zero(&out, 18);
		xs_reply_send(client, &out);
		break;
	case 25: // GetCursorImageAndName
		get_cursor_image(client, true);
		break;
	case 28:
		expand(client, in);
		break;
	case 34: // GetClientDisconnectMode
		xs_reply_begin(client, &out, 0);
		xout32(&out, 0);
		xs_reply_send(client, &out);
		break;
	// accepted, nothing to keep: the save set (flame doesn't reparent),
	// cursor input and names, changing and hiding cursors, barriers, the
	// disconnect mode
	case 1: case 3: case 23: case 26: case 27: case 29: case 30: case 31: case 32: case 33:
		break;
	default:
		xs_error(client, XS_ERROR_REQUEST, 0, XS_XFIXES_OPCODE, minor);
		break;
	}
}
