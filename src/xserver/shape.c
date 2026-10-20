#include <stdlib.h>

#include "xserver/internal.h"

// SHAPE 1.1: windows that aren't rectangles. the bounding and clip shapes
// are kept and told back, the input shape decides what the pointer finds
// (click-through overlays, toolkits' shadows). regions are window coords,
// from its inside corner

// who wants ShapeNotify for a window
struct shape_select {
	struct wl_list link; // flame_xserver.shape_selects
	struct xs_client *client;
	uint32_t window;
};

enum {
	KIND_BOUNDING,
	KIND_CLIP,
	KIND_INPUT,
};

enum {
	OP_SET,
	OP_UNION,
	OP_INTERSECT,
	OP_SUBTRACT,
	OP_INVERT,
};

// what a window is without a shape of that kind
static void default_shape(struct xs_window *window, int kind, pixman_region32_t *out) {
	int b = kind == KIND_CLIP ? 0 : window->border;
	pixman_region32_init_rect(out, -b, -b, (unsigned)(window->width + 2 * b),
		(unsigned)(window->height + 2 * b));
}

// the shape there is now, as a region the caller finishes
static void current(struct xs_window *window, int kind, pixman_region32_t *out) {
	if (window->shapes[kind]) {
		pixman_region32_init(out);
		pixman_region32_copy(out, window->shapes[kind]);
	} else {
		default_shape(window, kind, out);
	}
}

static void notify(struct flame_xserver *xserver, struct xs_window *window, int kind) {
	pixman_region32_t shape;
	current(window, kind, &shape);
	pixman_box32_t *extents = pixman_region32_extents(&shape);
	struct shape_select *select;
	wl_list_for_each(select, &xserver->shape_selects, link) {
		if (select->window != window->id) {
			continue;
		}
		struct xs_event event = {0};
		xs_event_put8(&event, 0, XS_SHAPE_FIRST_EVENT);
		xs_event_put8(&event, 1, (uint8_t)kind);
		xs_event_put32(&event, 4, window->id);
		xs_event_put16(&event, 8, (uint16_t)extents->x1);
		xs_event_put16(&event, 10, (uint16_t)extents->y1);
		xs_event_put16(&event, 12, (uint16_t)(extents->x2 - extents->x1));
		xs_event_put16(&event, 14, (uint16_t)(extents->y2 - extents->y1));
		xs_event_put32(&event, 16, xs_now(xserver));
		xs_event_put8(&event, 20, window->shapes[kind] != NULL);
		xs_event_send(select->client, &event);
	}
	pixman_region32_fini(&shape);
}

// what the client asked for, region in window coords, applied with op.
// src NULL: none, the shape goes back to the window's rectangle
static void apply(struct flame_xserver *xserver, struct xs_window *window, int kind, int op,
		pixman_region32_t *src) {
	if (!src) {
		if (window->shapes[kind]) {
			pixman_region32_fini(window->shapes[kind]);
			free(window->shapes[kind]);
			window->shapes[kind] = NULL;
		}
		notify(xserver, window, kind);
		xs_changed(xserver);
		return;
	}
	pixman_region32_t *result = malloc(sizeof(*result));
	if (!result) {
		return;
	}
	pixman_region32_init(result);
	pixman_region32_t dest;
	current(window, kind, &dest);
	switch (op) {
	case OP_SET:
		pixman_region32_copy(result, src);
		break;
	case OP_UNION:
		pixman_region32_union(result, &dest, src);
		break;
	case OP_INTERSECT:
		pixman_region32_intersect(result, &dest, src);
		break;
	case OP_SUBTRACT:
		pixman_region32_subtract(result, &dest, src);
		break;
	default: { // invert: src minus dest
		pixman_region32_subtract(result, src, &dest);
		break;
	}
	}
	pixman_region32_fini(&dest);
	if (window->shapes[kind]) {
		pixman_region32_fini(window->shapes[kind]);
		free(window->shapes[kind]);
	}
	window->shapes[kind] = result;
	notify(xserver, window, kind);
	xs_changed(xserver);
}

static struct xs_window *window_arg(struct xs_client *client, const struct xin *in, size_t at,
		uint8_t minor) {
	uint32_t id = xin32(in, at);
	struct xs_window *window = xserver_window(client->xserver, id);
	if (!window) {
		xs_error(client, XS_ERROR_WINDOW, id, XS_SHAPE_OPCODE, minor);
	}
	return window;
}

static bool kind_ok(struct xs_client *client, uint8_t kind, uint8_t minor) {
	if (kind > KIND_INPUT) {
		xs_error(client, XS_ERROR_VALUE, kind, XS_SHAPE_OPCODE, minor);
		return false;
	}
	return true;
}

static void rectangles(struct xs_client *client, const struct xin *in) {
	uint8_t op = xin8(in, 4), kind = xin8(in, 5);
	struct xs_window *window = window_arg(client, in, 8, 1);
	if (!window || !kind_ok(client, kind, 1)) {
		return;
	}
	if (op > OP_INVERT) {
		xs_error(client, XS_ERROR_VALUE, op, XS_SHAPE_OPCODE, 1);
		return;
	}
	int dx = xin16s(in, 12), dy = xin16s(in, 14);
	size_t count = in->len > 16 ? (in->len - 16) / 8 : 0;
	pixman_box32_t *boxes = count ? calloc(count, sizeof(*boxes)) : NULL;
	size_t used = 0;
	for (size_t i = 0; boxes && i < count; i++) {
		int x = xin16s(in, 16 + i * 8) + dx, y = xin16s(in, 18 + i * 8) + dy;
		int w = xin16(in, 20 + i * 8), h = xin16(in, 22 + i * 8);
		if (w && h) {
			boxes[used++] = (pixman_box32_t){x, y, x + w, y + h};
		}
	}
	pixman_region32_t region;
	pixman_region32_init_rects(&region, boxes, (int)used);
	free(boxes);
	apply(client->xserver, window, kind, op, &region);
	pixman_region32_fini(&region);
}

static void mask(struct xs_client *client, const struct xin *in) {
	uint8_t op = xin8(in, 4), kind = xin8(in, 5);
	struct xs_window *window = window_arg(client, in, 8, 2);
	if (!window || !kind_ok(client, kind, 2)) {
		return;
	}
	uint32_t source = xin32(in, 16);
	if (!source) {
		apply(client->xserver, window, kind, op, NULL);
		return;
	}
	struct xs_pixmap *pixmap = xs_pixmap_find(client->xserver, source);
	if (!pixmap || !pixmap->image || pixmap->depth != 1) {
		xs_error(client, pixmap ? XS_ERROR_MATCH : XS_ERROR_PIXMAP, source, XS_SHAPE_OPCODE, 2);
		return;
	}
	pixman_region32_t region;
	pixman_region32_init_from_image(&region, pixmap->image);
	pixman_region32_translate(&region, xin16s(in, 12), xin16s(in, 14));
	apply(client->xserver, window, kind, op, &region);
	pixman_region32_fini(&region);
}

static void combine(struct xs_client *client, const struct xin *in) {
	uint8_t op = xin8(in, 4), kind = xin8(in, 5), source_kind = xin8(in, 6);
	struct xs_window *window = window_arg(client, in, 8, 3);
	struct xs_window *source = window ? window_arg(client, in, 16, 3) : NULL;
	if (!source || !kind_ok(client, kind, 3) || !kind_ok(client, source_kind, 3)) {
		return;
	}
	pixman_region32_t region;
	current(source, source_kind, &region);
	// the source's shape where the source is, relative to the dest
	int sx, sy, dx, dy;
	xs_window_origin(source, &sx, &sy);
	xs_window_origin(window, &dx, &dy);
	pixman_region32_translate(&region, sx - dx + xin16s(in, 12), sy - dy + xin16s(in, 14));
	apply(client->xserver, window, kind, op, &region);
	pixman_region32_fini(&region);
}

static void offset(struct xs_client *client, const struct xin *in) {
	uint8_t kind = xin8(in, 4);
	struct xs_window *window = window_arg(client, in, 8, 4);
	if (!window || !kind_ok(client, kind, 4)) {
		return;
	}
	if (window->shapes[kind]) {
		pixman_region32_translate(window->shapes[kind], xin16s(in, 12), xin16s(in, 14));
		notify(client->xserver, window, kind);
		xs_changed(client->xserver);
	}
}

static void put_extents(struct xout *out, struct xs_window *window, int kind) {
	pixman_region32_t shape;
	current(window, kind, &shape);
	pixman_box32_t *e = pixman_region32_extents(&shape);
	xout16(out, (uint16_t)e->x1);
	xout16(out, (uint16_t)e->y1);
	xout16(out, (uint16_t)(e->x2 - e->x1));
	xout16(out, (uint16_t)(e->y2 - e->y1));
	pixman_region32_fini(&shape);
}

static void query_extents(struct xs_client *client, const struct xin *in) {
	struct xs_window *window = window_arg(client, in, 4, 5);
	if (!window) {
		return;
	}
	struct xout out;
	xs_reply_begin(client, &out, 0);
	xout8(&out, window->shapes[KIND_BOUNDING] != NULL);
	xout8(&out, window->shapes[KIND_CLIP] != NULL);
	xout16(&out, 0);
	put_extents(&out, window, KIND_BOUNDING);
	put_extents(&out, window, KIND_CLIP);
	xs_reply_send(client, &out);
}

static struct shape_select *find_select(struct flame_xserver *xserver,
		struct xs_client *client, uint32_t window) {
	struct shape_select *select;
	wl_list_for_each(select, &xserver->shape_selects, link) {
		if (select->client == client && select->window == window) {
			return select;
		}
	}
	return NULL;
}

static void select_input(struct xs_client *client, const struct xin *in) {
	struct xs_window *window = window_arg(client, in, 4, 6);
	if (!window) {
		return;
	}
	struct shape_select *select = find_select(client->xserver, client, window->id);
	if (xin8(in, 8) && !select) {
		select = calloc(1, sizeof(*select));
		if (!select) {
			xs_error(client, XS_ERROR_ALLOC, 0, XS_SHAPE_OPCODE, 6);
			return;
		}
		select->client = client;
		select->window = window->id;
		wl_list_insert(&client->xserver->shape_selects, &select->link);
	} else if (!xin8(in, 8) && select) {
		wl_list_remove(&select->link);
		free(select);
	}
}

static void get_rectangles(struct xs_client *client, const struct xin *in) {
	struct xs_window *window = window_arg(client, in, 4, 8);
	uint8_t kind = xin8(in, 8);
	if (!window || !kind_ok(client, kind, 8)) {
		return;
	}
	pixman_region32_t shape;
	current(window, kind, &shape);
	int n;
	pixman_box32_t *boxes = pixman_region32_rectangles(&shape, &n);
	struct xout out;
	xs_reply_begin(client, &out, 3); // YXBanded, what pixman's regions are
	xout32(&out, (uint32_t)n);
	xout_zero(&out, 20);
	for (int i = 0; i < n; i++) {
		xout16(&out, (uint16_t)boxes[i].x1);
		xout16(&out, (uint16_t)boxes[i].y1);
		xout16(&out, (uint16_t)(boxes[i].x2 - boxes[i].x1));
		xout16(&out, (uint16_t)(boxes[i].y2 - boxes[i].y1));
	}
	pixman_region32_fini(&shape);
	xs_reply_send(client, &out);
}

void xs_shape_request(struct xs_client *client, const struct xin *in) {
	struct xout out;
	uint8_t minor = xin8(in, 1);
	switch (minor) {
	case 0: // QueryVersion: 1.1
		xs_reply_begin(client, &out, 0);
		xout16(&out, 1);
		xout16(&out, 1);
		xs_reply_send(client, &out);
		break;
	case 1:
		rectangles(client, in);
		break;
	case 2:
		mask(client, in);
		break;
	case 3:
		combine(client, in);
		break;
	case 4:
		offset(client, in);
		break;
	case 5:
		query_extents(client, in);
		break;
	case 6:
		select_input(client, in);
		break;
	case 7: { // InputSelected
		struct xs_window *window = window_arg(client, in, 4, 7);
		if (window) {
			xs_reply_begin(client, &out,
				find_select(client->xserver, client, window->id) != NULL);
			xs_reply_send(client, &out);
		}
		break;
	}
	case 8:
		get_rectangles(client, in);
		break;
	default:
		xs_error(client, XS_ERROR_REQUEST, 0, XS_SHAPE_OPCODE, minor);
		break;
	}
}

bool xs_shape_contains(const struct xs_window *window, int x, int y) {
	for (int kind = KIND_BOUNDING; kind <= KIND_INPUT; kind += KIND_INPUT) {
		if (window->shapes[kind]
				&& !pixman_region32_contains_point(window->shapes[kind], x, y, NULL)) {
			return false;
		}
	}
	return true;
}

void xs_shape_window_gone(struct flame_xserver *xserver, struct xs_window *window) {
	for (int kind = 0; kind < 3; kind++) {
		if (window->shapes[kind]) {
			pixman_region32_fini(window->shapes[kind]);
			free(window->shapes[kind]);
			window->shapes[kind] = NULL;
		}
	}
	struct shape_select *select, *tmp;
	wl_list_for_each_safe(select, tmp, &xserver->shape_selects, link) {
		if (select->window == window->id) {
			wl_list_remove(&select->link);
			free(select);
		}
	}
}

void xs_shape_client_gone(struct flame_xserver *xserver, struct xs_client *client) {
	struct shape_select *select, *tmp;
	wl_list_for_each_safe(select, tmp, &xserver->shape_selects, link) {
		if (!client || select->client == client) {
			wl_list_remove(&select->link);
			free(select);
		}
	}
}
