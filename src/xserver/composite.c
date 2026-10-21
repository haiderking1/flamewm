#include <stdlib.h>

#include "xserver/internal.h"

// Composite 0.4. flame is the compositor: every top-level already lives in
// pixels of its own (its backing) and flame puts them on screen, the way
// rootless Xwayland redirects them all itself. so top-levels count as
// redirected from the start, a window's pixmap is a view of its backing,
// and redirecting is kept the way Xorg keeps it (who, how, the errors) but
// nothing a client redirects stops showing: a compositing manager can't
// take X windows off flame's screen. the overlay window is flame's and
// never shows

#define AUTOMATIC 0
#define MANUAL 1

struct redirect {
	struct wl_list link; // flame_xserver.redirects
	struct xs_client *client;
	uint32_t window;
	bool subwindows;
	uint8_t update;
};

// who holds the overlay window, it goes when nobody does
struct overlay_ref {
	struct wl_list link; // flame_xserver.overlay_refs
	struct xs_client *client;
};

static struct redirect *find(struct flame_xserver *xserver, struct xs_client *client,
		uint32_t window, bool subwindows) {
	struct redirect *r;
	wl_list_for_each(r, &xserver->redirects, link) {
		if (r->window == window && r->subwindows == subwindows
				&& (!client || r->client == client)) {
			return r;
		}
	}
	return NULL;
}

static bool manual_held(struct flame_xserver *xserver, uint32_t window, bool subwindows) {
	struct redirect *r;
	wl_list_for_each(r, &xserver->redirects, link) {
		if (r->window == window && r->subwindows == subwindows && r->update == MANUAL) {
			return true;
		}
	}
	return false;
}

// redirected by anyone: itself, or as a child of a window whose subwindows
// are. top-levels always are
static bool redirected(struct flame_xserver *xserver, struct xs_window *window) {
	if (!window->parent) {
		return false;
	}
	if (!window->parent->parent || find(xserver, NULL, window->id, false)
			|| find(xserver, NULL, window->parent->id, true)) {
		return true;
	}
	return false;
}

static void redirect(struct xs_client *client, const struct xin *in, bool subwindows,
		uint8_t minor) {
	struct flame_xserver *xserver = client->xserver;
	uint32_t id = xin32(in, 4);
	uint8_t update = xin8(in, 8);
	struct xs_window *window = xserver_window(xserver, id);
	if (!window) {
		xs_error(client, XS_ERROR_WINDOW, id, XS_COMPOSITE_OPCODE, minor);
		return;
	}
	if (update > MANUAL) {
		xs_error(client, XS_ERROR_VALUE, update, XS_COMPOSITE_OPCODE, minor);
		return;
	}
	if (!subwindows && !window->parent) {
		xs_error(client, XS_ERROR_MATCH, id, XS_COMPOSITE_OPCODE, minor);
		return; // the root itself can't be
	}
	// twice by the same client, or a second manual one: Xorg says BadAccess
	if (find(xserver, client, id, subwindows)
			|| (update == MANUAL && manual_held(xserver, id, subwindows))) {
		xs_error(client, XS_ERROR_ACCESS, id, XS_COMPOSITE_OPCODE, minor);
		return;
	}
	struct redirect *r = calloc(1, sizeof(*r));
	if (!r) {
		xs_error(client, XS_ERROR_ALLOC, id, XS_COMPOSITE_OPCODE, minor);
		return;
	}
	r->client = client;
	r->window = id;
	r->subwindows = subwindows;
	r->update = update;
	wl_list_insert(&xserver->redirects, &r->link);
}

static void unredirect(struct xs_client *client, const struct xin *in, bool subwindows,
		uint8_t minor) {
	uint32_t id = xin32(in, 4);
	if (!xserver_window(client->xserver, id)) {
		xs_error(client, XS_ERROR_WINDOW, id, XS_COMPOSITE_OPCODE, minor);
		return;
	}
	struct redirect *r = find(client->xserver, client, id, subwindows);
	if (!r) {
		xs_error(client, XS_ERROR_VALUE, id, XS_COMPOSITE_OPCODE, minor);
		return;
	}
	wl_list_remove(&r->link);
	free(r);
}

// a window's outer rectangle, root coords
static pixman_box32_t outer(const struct xs_window *window) {
	int x, y;
	xs_window_origin(window, &x, &y);
	int b = window->border;
	return (pixman_box32_t){x - b, y - b, x + window->width + b, y + window->height + b};
}

// what of the window, border included, isn't cut off by its parents or
// covered by windows above it: window coords from its inside corner
static void border_clip(struct xs_window *window, pixman_region32_t *out) {
	pixman_region32_init(out);
	if (!window->parent || !xs_window_viewable(window)) {
		return;
	}
	pixman_box32_t box = outer(window);
	pixman_region32_init_rects(out, &box, 1);
	for (struct xs_window *w = window; w->parent; w = w->parent) {
		// inside the parent
		if (w->parent->parent) {
			int px, py;
			xs_window_origin(w->parent, &px, &py);
			pixman_region32_intersect_rect(out, out, px, py, w->parent->width,
				w->parent->height);
		}
		// not under the siblings stacked above
		for (struct xs_window *above = wl_container_of(w->link.next, above, link);
				&above->link != &w->parent->children;
				above = wl_container_of(above->link.next, above, link)) {
			if (!above->mapped) {
				continue;
			}
			pixman_box32_t a = outer(above);
			pixman_region32_t cover;
			pixman_region32_init_rects(&cover, &a, 1);
			pixman_region32_subtract(out, out, &cover);
			pixman_region32_fini(&cover);
		}
	}
	int x, y;
	xs_window_origin(window, &x, &y);
	pixman_region32_translate(out, -x, -y);
}

static void region_from_border_clip(struct xs_client *client, const struct xin *in) {
	uint32_t region_id = xin32(in, 4), id = xin32(in, 8);
	struct xs_window *window = xserver_window(client->xserver, id);
	if (!window) {
		xs_error(client, XS_ERROR_WINDOW, id, XS_COMPOSITE_OPCODE, 5);
		return;
	}
	if (!xs_id_available(client, region_id)) {
		xs_error(client, XS_ERROR_ID_CHOICE, region_id, XS_COMPOSITE_OPCODE, 5);
		return;
	}
	struct xs_region *region = calloc(1, sizeof(*region));
	if (!region) {
		xs_error(client, XS_ERROR_ALLOC, region_id, XS_COMPOSITE_OPCODE, 5);
		return;
	}
	region->id = region_id;
	region->owner = client;
	border_clip(window, &region->region);
	wl_list_insert(&client->xserver->regions, &region->link);
}

static void unref_backing(pixman_image_t *image, void *data) {
	pixman_image_unref(data);
}

// the window's pixels, border included, as a pixmap: a view of its
// top-level's backing that keeps it alive. a resize gives the window new
// pixels, the pixmap keeps the old ones (as in Xorg). parts the backing
// doesn't have (a top-level's border) come as a copy instead
static void name_window_pixmap(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	uint32_t id = xin32(in, 4), pixmap_id = xin32(in, 8);
	struct xs_window *window = xserver_window(xserver, id);
	if (!window) {
		xs_error(client, XS_ERROR_WINDOW, id, XS_COMPOSITE_OPCODE, 6);
		return;
	}
	struct xs_window *top = xs_window_top(window);
	if (!top || !top->backing || window->class != 1 || !xs_window_viewable(window)
			|| !redirected(xserver, window)) {
		xs_error(client, XS_ERROR_MATCH, id, XS_COMPOSITE_OPCODE, 6);
		return;
	}
	if (!xs_id_available(client, pixmap_id)) {
		xs_error(client, XS_ERROR_ID_CHOICE, pixmap_id, XS_COMPOSITE_OPCODE, 6);
		return;
	}
	int tx, ty, wx, wy;
	xs_window_origin(top, &tx, &ty);
	xs_window_origin(window, &wx, &wy);
	int b = window->border;
	int x = wx - tx - b, y = wy - ty - b;
	int w = window->width + 2 * b, h = window->height + 2 * b;
	pixman_image_t *backing = top->backing;
	pixman_format_code_t format = pixman_image_get_format(backing);
	int bw = pixman_image_get_width(backing), bh = pixman_image_get_height(backing);
	pixman_image_t *image;
	if (x >= 0 && y >= 0 && x + w <= bw && y + h <= bh) {
		int stride = pixman_image_get_stride(backing);
		uint8_t *bits = (uint8_t *)pixman_image_get_data(backing) + (size_t)y * (size_t)stride
			+ (size_t)x * 4;
		image = pixman_image_create_bits(format, w, h, (uint32_t *)bits, stride);
		if (image) {
			pixman_image_set_destroy_function(image, unref_backing, pixman_image_ref(backing));
		}
	} else {
		image = xs_image_create(window->depth == 32 ? 32 : 24, w, h, NULL, 0);
		if (image) {
			pixman_image_composite32(PIXMAN_OP_SRC, backing, NULL, image, x, y, 0, 0, 0, 0, w,
				h);
		}
	}
	struct xs_pixmap *pixmap = image ? calloc(1, sizeof(*pixmap)) : NULL;
	if (!pixmap) {
		if (image) {
			pixman_image_unref(image);
		}
		xs_error(client, XS_ERROR_ALLOC, pixmap_id, XS_COMPOSITE_OPCODE, 6);
		return;
	}
	pixmap->id = pixmap_id;
	pixmap->owner = client;
	pixmap->depth = window->depth;
	pixmap->bpp = 32;
	pixmap->width = (uint16_t)w;
	pixmap->height = (uint16_t)h;
	pixmap->image = image;
	wl_list_insert(&xserver->pixmaps, &pixmap->link);
}

static void overlay_drop(struct flame_xserver *xserver, struct overlay_ref *ref) {
	wl_list_remove(&ref->link);
	free(ref);
	if (wl_list_empty(&xserver->overlay_refs) && xserver->overlay) {
		// cleared first: its going comes back through xs_composite_window_gone
		struct xs_window *overlay = xserver->overlay;
		xserver->overlay = NULL;
		xs_window_destroy(xserver, overlay);
	}
}

static void get_overlay(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	uint32_t id = xin32(in, 4);
	if (!xserver_window(xserver, id)) {
		xs_error(client, XS_ERROR_WINDOW, id, XS_COMPOSITE_OPCODE, 7);
		return;
	}
	if (!xserver->overlay) {
		xserver->overlay = xserver_flame_window_create(xserver);
		if (xserver->overlay) {
			// mapped by a client, still nothing flame would manage or show
			xserver->overlay->override_redirect = true;
		}
	}
	struct overlay_ref *ref = xserver->overlay ? calloc(1, sizeof(*ref)) : NULL;
	if (!ref) {
		xs_error(client, XS_ERROR_ALLOC, id, XS_COMPOSITE_OPCODE, 7);
		return;
	}
	ref->client = client;
	wl_list_insert(&xserver->overlay_refs, &ref->link);
	struct xout out;
	xs_reply_begin(client, &out, 0);
	xout32(&out, xserver->overlay->id);
	xs_reply_send(client, &out);
}

static void release_overlay(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	uint32_t id = xin32(in, 4);
	if (!xserver_window(xserver, id)) {
		xs_error(client, XS_ERROR_WINDOW, id, XS_COMPOSITE_OPCODE, 8);
		return;
	}
	struct overlay_ref *ref;
	wl_list_for_each(ref, &xserver->overlay_refs, link) {
		if (ref->client == client) {
			overlay_drop(xserver, ref);
			return;
		}
	}
}

void xs_composite_request(struct xs_client *client, const struct xin *in) {
	struct xout out;
	uint8_t minor = xin8(in, 1);
	switch (minor) {
	case 0: // QueryVersion: 0.4
		xs_reply_begin(client, &out, 0);
		xout32(&out, 0);
		xout32(&out, 4);
		xs_reply_send(client, &out);
		break;
	case 1:
		redirect(client, in, false, minor);
		break;
	case 2:
		redirect(client, in, true, minor);
		break;
	case 3:
		unredirect(client, in, false, minor);
		break;
	case 4:
		unredirect(client, in, true, minor);
		break;
	case 5:
		region_from_border_clip(client, in);
		break;
	case 6:
		name_window_pixmap(client, in);
		break;
	case 7:
		get_overlay(client, in);
		break;
	case 8:
		release_overlay(client, in);
		break;
	default:
		xs_error(client, XS_ERROR_REQUEST, 0, XS_COMPOSITE_OPCODE, minor);
		break;
	}
}

void xs_composite_window_gone(struct flame_xserver *xserver, uint32_t window) {
	struct redirect *r, *tmp;
	wl_list_for_each_safe(r, tmp, &xserver->redirects, link) {
		if (r->window == window) {
			wl_list_remove(&r->link);
			free(r);
		}
	}
	if (xserver->overlay && xserver->overlay->id == window) {
		// gone some other way (flame going down): nobody holds it anymore
		xserver->overlay = NULL;
		struct overlay_ref *ref, *rtmp;
		wl_list_for_each_safe(ref, rtmp, &xserver->overlay_refs, link) {
			wl_list_remove(&ref->link);
			free(ref);
		}
	}
}

void xs_composite_client_gone(struct flame_xserver *xserver, struct xs_client *client) {
	struct redirect *r, *tmp;
	wl_list_for_each_safe(r, tmp, &xserver->redirects, link) {
		if (!client || r->client == client) {
			wl_list_remove(&r->link);
			free(r);
		}
	}
	struct overlay_ref *ref, *rtmp;
	wl_list_for_each_safe(ref, rtmp, &xserver->overlay_refs, link) {
		if (!client || ref->client == client) {
			overlay_drop(xserver, ref);
		}
	}
}
