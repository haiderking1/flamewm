#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/util/log.h>

#include "xserver/internal.h"

// drawing with the cpu: what apps that don't go through the gpu (DRI3 and
// Present) draw with. pixmaps hold pixman images, a top-level's whole tree
// draws into one backing image (each window at its offset), and flame
// shows the backing (xshell/) once the damage idle hands it over.
//
// every shape becomes a pixman region first (rectangles, the spans of a
// polygon, a line's pixels, an arc as a polygon), clipped to the drawable
// and the GC; one painter then fills it the way the GC says: solid with
// its raster op, tiled, or stippled. no core fonts: text requests draw
// nothing, toolkits draw their text into images on their own anyway

#define MAX_IMAGE_BYTES (256u << 20)
#define PI 3.14159265358979323846

// GXcopy and the other raster ops, by GC function
enum {
	GX_CLEAR,
	GX_AND,
	GX_AND_REVERSE,
	GX_COPY,
	GX_AND_INVERTED,
	GX_NOOP,
	GX_XOR,
	GX_OR,
	GX_NOR,
	GX_EQUIV,
	GX_INVERT,
	GX_OR_REVERSE,
	GX_COPY_INVERTED,
	GX_OR_INVERTED,
	GX_NAND,
	GX_SET,
};

enum {
	FILL_SOLID,
	FILL_TILED,
	FILL_STIPPLED,
	FILL_OPAQUE_STIPPLED,
};

// ---- images ----

static pixman_format_code_t depth_format(int depth) {
	switch (depth) {
	case 1:
		return PIXMAN_a1;
	case 4: // a byte a pixel, like Xorg: libXrender insists on the depth existing
	case 8:
		return PIXMAN_a8;
	case 24:
		return PIXMAN_x8r8g8b8;
	case 32:
		return PIXMAN_a8r8g8b8;
	default:
		return 0;
	}
}

static int depth_bpp(int depth) {
	return depth == 1 ? 1 : depth <= 8 ? 8 : 32;
}

size_t xs_image_stride(int depth, int width) {
	return ((size_t)width * (size_t)depth_bpp(depth) + 31) / 32 * 4;
}

pixman_image_t *xs_image_create(int depth, int width, int height, void *bits, size_t stride) {
	pixman_format_code_t format = depth_format(depth);
	if (!format || width <= 0 || height <= 0 || width > 32767 || height > 32767) {
		return NULL;
	}
	if (!bits) {
		stride = xs_image_stride(depth, width);
		if (stride * (size_t)height > MAX_IMAGE_BYTES) {
			return NULL;
		}
	}
	// pixman clears what it allocates itself
	return pixman_image_create_bits(format, width, height, bits, (int)stride);
}

static uint32_t read_pixel(pixman_image_t *image, int x, int y) {
	uint8_t *row = (uint8_t *)pixman_image_get_data(image)
		+ (size_t)y * (size_t)pixman_image_get_stride(image);
	switch (PIXMAN_FORMAT_BPP(pixman_image_get_format(image))) {
	case 1:
		return (((uint32_t *)row)[x >> 5] >> (x & 31)) & 1;
	case 8:
		return row[x];
	default:
		return ((uint32_t *)row)[x];
	}
}

static void write_pixel(pixman_image_t *image, int x, int y, uint32_t value) {
	uint8_t *row = (uint8_t *)pixman_image_get_data(image)
		+ (size_t)y * (size_t)pixman_image_get_stride(image);
	switch (PIXMAN_FORMAT_BPP(pixman_image_get_format(image))) {
	case 1: {
		uint32_t *word = &((uint32_t *)row)[x >> 5];
		*word = (*word & ~(1u << (x & 31))) | ((value & 1) << (x & 31));
		break;
	}
	case 8:
		row[x] = (uint8_t)value;
		break;
	default:
		((uint32_t *)row)[x] = value;
		break;
	}
}

static uint32_t raster_op(uint8_t function, uint32_t src, uint32_t dst) {
	switch (function) {
	case GX_CLEAR:
		return 0;
	case GX_AND:
		return src & dst;
	case GX_AND_REVERSE:
		return src & ~dst;
	case GX_COPY:
		return src;
	case GX_AND_INVERTED:
		return ~src & dst;
	case GX_NOOP:
		return dst;
	case GX_XOR:
		return src ^ dst;
	case GX_OR:
		return src | dst;
	case GX_NOR:
		return ~(src | dst);
	case GX_EQUIV:
		return ~src ^ dst;
	case GX_INVERT:
		return ~dst;
	case GX_OR_REVERSE:
		return src | ~dst;
	case GX_COPY_INVERTED:
		return ~src;
	case GX_OR_INVERTED:
		return ~src | dst;
	case GX_NAND:
		return ~(src & dst);
	default:
		return ~0u;
	}
}

// a pixel value as pixman wants a color. depth 24 is opaque whatever the top
// byte says, depth 32 carries its alpha
static pixman_color_t pixel_color(uint32_t pixel, int depth) {
	pixman_color_t color = {
		.red = (uint16_t)(((pixel >> 16) & 0xff) * 257),
		.green = (uint16_t)(((pixel >> 8) & 0xff) * 257),
		.blue = (uint16_t)((pixel & 0xff) * 257),
		.alpha = depth == 32 ? (uint16_t)(((pixel >> 24) & 0xff) * 257) : 0xffff,
	};
	if (depth == 1 || depth == 8) {
		uint16_t a = depth == 1 ? ((pixel & 1) ? 0xffff : 0) : (uint16_t)((pixel & 0xff) * 257);
		color = (pixman_color_t){.alpha = a};
	}
	return color;
}

static uint32_t stored_pixel(uint32_t pixel, int depth) {
	switch (depth) {
	case 1:
		return pixel & 1;
	case 8:
		return pixel & 0xff;
	case 24:
		return pixel | 0xff000000u;
	default:
		return pixel;
	}
}

// ---- lookups ----

struct xs_pixmap *xs_pixmap_find(struct flame_xserver *xserver, uint32_t id) {
	struct xs_pixmap *pixmap;
	wl_list_for_each(pixmap, &xserver->pixmaps, link) {
		if (pixmap->id == id) {
			return pixmap;
		}
	}
	return NULL;
}

struct xs_gc *xs_gc_find(struct flame_xserver *xserver, uint32_t id) {
	struct xs_gc *gc;
	wl_list_for_each(gc, &xserver->gcs, link) {
		if (gc->id == id) {
			return gc;
		}
	}
	return NULL;
}

static struct xs_gc *get_gc(struct xs_client *client, uint32_t id, uint8_t major,
		uint16_t minor) {
	struct xs_gc *gc = xs_gc_find(client->xserver, id);
	if (!gc) {
		xs_error(client, XS_ERROR_GC, id, major, minor);
	}
	return gc;
}

// the top-level a window is in, NULL for the root
struct xs_window *xs_window_top(struct xs_window *window) {
	if (!window || !window->parent) {
		return NULL;
	}
	while (window->parent->parent) {
		window = window->parent;
	}
	return window;
}

// ---- damage, handed to flame once per loop iteration ----

static void handle_damage_idle(void *data) {
	struct flame_xserver *xserver = data;
	xserver->damage_idle = NULL;
	struct xs_window *window;
	wl_list_for_each(window, &xserver->windows, all_link) {
		if (!window->damaged || !window->painted) {
			continue; // not painted yet: its damage keeps adding up
		}
		window->damaged = false;
		if (window->backing && xserver->hooks.damage) {
			// a window bigger than its backing drew past it
			pixman_region32_intersect_rect(&window->damage, &window->damage, 0, 0,
				(unsigned)pixman_image_get_width(window->backing),
				(unsigned)pixman_image_get_height(window->backing));
			xserver->hooks.damage(xserver->hooks.data, window, &window->damage);
		}
		pixman_region32_clear(&window->damage);
	}
}

// what changed, handed over once the window has been painted. the
// background alone (None leaves the new backing black) isn't worth showing:
// a menu would flash black until the app draws it
static void damage_quiet(struct flame_xserver *xserver, struct xs_window *top,
		const pixman_region32_t *region);

// the app drew: from now on the window shows
static void damage(struct flame_xserver *xserver, struct xs_window *top,
		const pixman_region32_t *region) {
	if (top && top->backing) {
		top->painted = true;
	}
	damage_quiet(xserver, top, region);
}

static void damage_quiet(struct flame_xserver *xserver, struct xs_window *top,
		const pixman_region32_t *region) {
	if (!top || !top->backing || !pixman_region32_not_empty(region)) {
		return;
	}
	xs_damage_backing(xserver, top, region);
	pixman_region32_union(&top->damage, &top->damage, region);
	top->damaged = true;
	if (!xserver->damage_idle) {
		xserver->damage_idle = wl_event_loop_add_idle(xserver->loop, handle_damage_idle, xserver);
	}
}

// ---- where drawing goes ----

struct target {
	pixman_image_t *image;
	int dx, dy; // the drawable's 0,0 in image coords
	int width, height, depth;
	pixman_region32_t clip; // image coords, what may be touched
	struct xs_window *window, *top; // windows: the window and its top-level
	struct xs_pixmap *pixmap; // pixmaps
};

// drawing landed in region (image coords): flame hears about windows, and
// DAMAGE's clients about either
static void damage_target(struct flame_xserver *xserver, const struct target *t,
		const pixman_region32_t *region) {
	if (t->pixmap) {
		xs_damage_pixmap(xserver, t->pixmap, region);
	} else {
		damage(xserver, t->top, region);
	}
}

static void target_finish(struct target *t) {
	pixman_region32_fini(&t->clip);
}

// a viewable window's part of its top-level's backing. children cut out
// unless inferiors are included
static bool target_window(struct xs_window *window, bool inferiors, struct target *t) {
	struct xs_window *top = xs_window_top(window);
	if (!top || !top->backing || window->class != 1 || !xs_window_viewable(window)) {
		return false;
	}
	int tx, ty, wx, wy;
	xs_window_origin(top, &tx, &ty);
	xs_window_origin(window, &wx, &wy);
	*t = (struct target){
		.image = top->backing,
		.dx = wx - tx,
		.dy = wy - ty,
		.width = window->width,
		.height = window->height,
		.depth = window->depth,
		.window = window,
		.top = top,
	};
	pixman_region32_init_rect(&t->clip, t->dx, t->dy, window->width, window->height);
	for (struct xs_window *up = window->parent; up && up->parent; up = up->parent) {
		int ux, uy;
		xs_window_origin(up, &ux, &uy);
		pixman_region32_intersect_rect(&t->clip, &t->clip, ux - tx, uy - ty, up->width,
			up->height);
	}
	if (!inferiors) {
		struct xs_window *child;
		wl_list_for_each(child, &window->children, link) {
			if (!child->mapped || child->class != 1) {
				continue;
			}
			pixman_region32_t cut;
			pixman_region32_init_rect(&cut, t->dx + child->x, t->dy + child->y,
				child->width + 2u * child->border, child->height + 2u * child->border);
			pixman_region32_subtract(&t->clip, &t->clip, &cut);
			pixman_region32_fini(&cut);
		}
	}
	pixman_region32_intersect_rect(&t->clip, &t->clip, 0, 0,
		pixman_image_get_width(top->backing), pixman_image_get_height(top->backing));
	return true;
}

// any drawable. false: nothing to draw into (unmapped window, a gpu pixmap),
// error says whether it doesn't exist at all
static bool target_get(struct xs_client *client, uint32_t id, const struct xs_gc *gc,
		struct target *t, bool *missing) {
	struct flame_xserver *xserver = client->xserver;
	*missing = false;
	struct xs_window *window = xserver_window(xserver, id);
	if (window) {
		if (!target_window(window, gc && gc->subwindow_mode == 1, t)) {
			return false;
		}
	} else {
		struct xs_pixmap *pixmap = xs_pixmap_find(xserver, id);
		if (!pixmap) {
			*missing = true;
			return false;
		}
		if (!pixmap->image) {
			return false; // a gpu buffer, the cpu doesn't draw into those
		}
		*t = (struct target){
			.image = pixmap->image,
			.width = pixmap->width,
			.height = pixmap->height,
			.depth = pixmap->depth,
			.pixmap = pixmap,
		};
		pixman_region32_init_rect(&t->clip, 0, 0, pixmap->width, pixmap->height);
	}
	if (gc && gc->clipped) {
		pixman_region32_t clip;
		pixman_region32_init(&clip);
		pixman_region32_copy(&clip, &gc->clip);
		pixman_region32_translate(&clip, t->dx + gc->clip_x, t->dy + gc->clip_y);
		pixman_region32_intersect(&t->clip, &t->clip, &clip);
		pixman_region32_fini(&clip);
	}
	return true;
}

// for drawing requests: the drawable, a BadDrawable when it doesn't exist
static bool target_for(struct xs_client *client, uint32_t id, const struct xs_gc *gc,
		struct target *t, uint8_t major, uint16_t minor) {
	bool missing;
	if (target_get(client, id, gc, t, &missing)) {
		return true;
	}
	if (missing) {
		xs_error(client, XS_ERROR_DRAWABLE, id, major, minor);
	}
	return false;
}

// for RENDER: a viewable window's pixels, where it is in them and what of
// it shows (backing coords)
bool xs_window_pixels(struct xs_window *window, bool inferiors, pixman_image_t **backing,
		int *dx, int *dy, pixman_region32_t *clip) {
	struct target t;
	if (!target_window(window, inferiors, &t)) {
		return false;
	}
	*backing = t.image;
	*dx = t.dx;
	*dy = t.dy;
	pixman_region32_copy(clip, &t.clip);
	target_finish(&t);
	return true;
}

void xs_damage(struct flame_xserver *xserver, struct xs_window *top,
		const pixman_region32_t *region) {
	damage(xserver, top, region);
}

// ---- shapes into regions ----

struct boxes {
	pixman_box32_t *data;
	size_t len, cap;
};

static void boxes_add(struct boxes *b, int x1, int y1, int x2, int y2) {
	if (x2 <= x1 || y2 <= y1) {
		return;
	}
	if (b->len == b->cap) {
		size_t cap = b->cap ? b->cap * 2 : 64;
		pixman_box32_t *data = realloc(b->data, cap * sizeof(*data));
		if (!data) {
			return;
		}
		b->data = data;
		b->cap = cap;
	}
	b->data[b->len++] = (pixman_box32_t){x1, y1, x2, y2};
}

// the boxes (drawable coords) as a region in image coords, clipped
static void boxes_region(struct boxes *b, const struct target *t, pixman_region32_t *out) {
	for (size_t i = 0; i < b->len; i++) {
		b->data[i].x1 += t->dx;
		b->data[i].x2 += t->dx;
		b->data[i].y1 += t->dy;
		b->data[i].y2 += t->dy;
	}
	pixman_region32_init_rects(out, b->data, (int)b->len);
	pixman_region32_intersect(out, out, &t->clip);
	free(b->data);
	*b = (struct boxes){0};
}

// a 1 pixel wide line, both ends included: bresenham's pixels, the ones
// next to each other on a row as one run
static void line_thin(struct boxes *b, int x0, int y0, int x1, int y1) {
	int dx = abs(x1 - x0), dy = -abs(y1 - y0);
	int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
	int err = dx + dy;
	int run_from = x0, run_to = x0, run_y = y0;
	for (;;) {
		if (y0 != run_y) {
			boxes_add(b, run_from < run_to ? run_from : run_to, run_y,
				(run_from < run_to ? run_to : run_from) + 1, run_y + 1);
			run_from = x0;
			run_y = y0;
		}
		run_to = x0;
		if (x0 == x1 && y0 == y1) {
			break;
		}
		int e2 = 2 * err;
		if (e2 >= dy) {
			err += dy;
			x0 += sx;
		}
		if (e2 <= dx) {
			err += dx;
			y0 += sy;
		}
	}
	boxes_add(b, run_from < run_to ? run_from : run_to, run_y,
		(run_from < run_to ? run_to : run_from) + 1, run_y + 1);
}

struct fpoint {
	double x, y;
};

// a polygon's inside as spans, sampled at pixel centers. winding or
// even-odd
static void polygon_fill(struct boxes *b, const struct fpoint *p, size_t n, bool winding) {
	if (n < 3) {
		return;
	}
	double ymin = p[0].y, ymax = p[0].y;
	for (size_t i = 1; i < n; i++) {
		ymin = p[i].y < ymin ? p[i].y : ymin;
		ymax = p[i].y > ymax ? p[i].y : ymax;
	}
	struct crossing {
		double x;
		int dir;
	} *xs = malloc(n * sizeof(*xs));
	if (!xs) {
		return;
	}
	for (int y = (int)floor(ymin); y < (int)ceil(ymax); y++) {
		double cy = y + 0.5;
		size_t count = 0;
		for (size_t i = 0; i < n; i++) {
			struct fpoint a = p[i], c = p[(i + 1) % n];
			if ((a.y <= cy && c.y > cy) || (c.y <= cy && a.y > cy)) {
				double x = a.x + (cy - a.y) * (c.x - a.x) / (c.y - a.y);
				xs[count++] = (struct crossing){x, a.y < c.y ? 1 : -1};
			}
		}
		// insertion sort, a handful per row
		for (size_t i = 1; i < count; i++) {
			struct crossing v = xs[i];
			size_t j = i;
			while (j > 0 && xs[j - 1].x > v.x) {
				xs[j] = xs[j - 1];
				j--;
			}
			xs[j] = v;
		}
		int wind = 0;
		for (size_t i = 0; i + 1 < count; i++) {
			wind += xs[i].dir;
			bool inside = winding ? wind != 0 : (i % 2) == 0;
			if (inside) {
				boxes_add(b, (int)ceil(xs[i].x - 0.5), y, (int)ceil(xs[i + 1].x - 0.5), y + 1);
			}
		}
	}
	free(xs);
}

// a line of width w (> 1) as the rectangle around it, butt ends
static void line_wide(struct boxes *b, int x0, int y0, int x1, int y1, int w) {
	double dx = x1 - x0, dy = y1 - y0;
	double len = sqrt(dx * dx + dy * dy);
	if (len == 0) {
		boxes_add(b, x0 - w / 2, y0 - w / 2, x0 - w / 2 + w, y0 - w / 2 + w);
		return;
	}
	double nx = -dy / len * w / 2.0, ny = dx / len * w / 2.0;
	struct fpoint quad[4] = {
		{x0 + 0.5 + nx, y0 + 0.5 + ny},
		{x1 + 0.5 + nx, y1 + 0.5 + ny},
		{x1 + 0.5 - nx, y1 + 0.5 - ny},
		{x0 + 0.5 - nx, y0 + 0.5 - ny},
	};
	polygon_fill(b, quad, 4, true);
}

static void line(struct boxes *b, const struct xs_gc *gc, int x0, int y0, int x1, int y1) {
	if (gc->line_width <= 1) {
		line_thin(b, x0, y0, x1, y1);
	} else {
		line_wide(b, x0, y0, x1, y1, gc->line_width);
	}
}

// an arc of the ellipse in x,y,w,h from angle1 over angle2 (64ths of a
// degree, counterclockwise from 3 o'clock) as points
static size_t arc_points(int x, int y, int w, int h, int angle1, int angle2,
		struct fpoint *out, size_t max) {
	double cx = x + w / 2.0, cy = y + h / 2.0, rx = w / 2.0, ry = h / 2.0;
	if (angle2 > 360 * 64) {
		angle2 = 360 * 64;
	} else if (angle2 < -360 * 64) {
		angle2 = -360 * 64;
	}
	double a1 = angle1 / 64.0 * PI / 180.0, a2 = angle2 / 64.0 * PI / 180.0;
	size_t steps = (size_t)((rx + ry) * fabs(a2) / 4) + 4;
	if (steps > max - 2) {
		steps = max - 2;
	}
	for (size_t i = 0; i <= steps; i++) {
		double a = a1 + a2 * (double)i / (double)steps;
		out[i] = (struct fpoint){cx + rx * cos(a), cy - ry * sin(a)};
	}
	return steps + 1;
}

// ---- painting ----

static void fill_solid(const struct target *t, const struct xs_gc *gc, uint32_t pixel,
		uint8_t function, const pixman_region32_t *region) {
	int n;
	const pixman_box32_t *box = pixman_region32_rectangles(region, &n);
	uint32_t value = stored_pixel(pixel, t->depth);
	uint32_t planes = gc ? gc->plane_mask : ~0u;
	if (function == GX_COPY && (planes | 0xff000000u) == ~0u) {
		uint32_t *bits = pixman_image_get_data(t->image);
		int stride = pixman_image_get_stride(t->image) / 4;
		int bpp = PIXMAN_FORMAT_BPP(pixman_image_get_format(t->image));
		for (int i = 0; i < n; i++) {
			pixman_fill(bits, stride, bpp, box[i].x1, box[i].y1, box[i].x2 - box[i].x1,
				box[i].y2 - box[i].y1, value);
		}
		return;
	}
	for (int i = 0; i < n; i++) {
		for (int y = box[i].y1; y < box[i].y2; y++) {
			for (int x = box[i].x1; x < box[i].x2; x++) {
				uint32_t dst = read_pixel(t->image, x, y);
				uint32_t out = raster_op(function, value, dst);
				out = (out & planes) | (dst & ~planes);
				write_pixel(t->image, x, y, stored_pixel(out, t->depth));
			}
		}
	}
}

static pixman_image_t *gc_pixmap(struct flame_xserver *xserver, uint32_t id) {
	struct xs_pixmap *pixmap = id ? xs_pixmap_find(xserver, id) : NULL;
	return pixmap ? pixmap->image : NULL;
}

// the region with the GC's fill. foreground for shapes, background for
// what opaque stipples and images leave
static void paint(struct flame_xserver *xserver, const struct target *t,
		const struct xs_gc *gc, const pixman_region32_t *region) {
	if (!pixman_region32_not_empty(region)) {
		return;
	}
	uint8_t style = gc->fill_style;
	pixman_image_t *tile = style == FILL_TILED ? gc_pixmap(xserver, gc->tile) : NULL;
	pixman_image_t *stipple = style == FILL_STIPPLED || style == FILL_OPAQUE_STIPPLED
		? gc_pixmap(xserver, gc->stipple) : NULL;
	int ox = t->dx + gc->ts_x, oy = t->dy + gc->ts_y;
	if (tile && pixman_image_get_depth(tile) == pixman_image_get_depth(t->image)) {
		pixman_image_set_repeat(tile, PIXMAN_REPEAT_NORMAL);
		pixman_image_set_clip_region32(t->image, (pixman_region32_t *)region);
		const pixman_box32_t *e = pixman_region32_extents((pixman_region32_t *)region);
		pixman_image_composite32(PIXMAN_OP_SRC, tile, NULL, t->image, e->x1 - ox, e->y1 - oy,
			0, 0, e->x1, e->y1, e->x2 - e->x1, e->y2 - e->y1);
		pixman_image_set_clip_region32(t->image, NULL);
		pixman_image_set_repeat(tile, PIXMAN_REPEAT_NONE);
	} else if (stipple && t->depth != 1) {
		if (style == FILL_OPAQUE_STIPPLED) {
			fill_solid(t, gc, gc->background, GX_COPY, region);
		}
		pixman_color_t color = pixel_color(gc->foreground, t->depth);
		pixman_image_t *src = pixman_image_create_solid_fill(&color);
		pixman_image_set_repeat(stipple, PIXMAN_REPEAT_NORMAL);
		pixman_image_set_clip_region32(t->image, (pixman_region32_t *)region);
		const pixman_box32_t *e = pixman_region32_extents((pixman_region32_t *)region);
		pixman_image_composite32(PIXMAN_OP_OVER, src, stipple, t->image, 0, 0, e->x1 - ox,
			e->y1 - oy, e->x1, e->y1, e->x2 - e->x1, e->y2 - e->y1);
		pixman_image_set_clip_region32(t->image, NULL);
		pixman_image_set_repeat(stipple, PIXMAN_REPEAT_NONE);
		pixman_image_unref(src);
	} else {
		fill_solid(t, gc, gc->foreground, gc->function, region);
	}
}

// paint the shape and tell flame
static void draw_boxes(struct xs_client *client, struct target *t, const struct xs_gc *gc,
		struct boxes *b) {
	pixman_region32_t region;
	boxes_region(b, t, &region);
	paint(client->xserver, t, gc, &region);
	damage_target(client->xserver, t, &region);
	pixman_region32_fini(&region);
}

// ---- backgrounds and exposures ----

// paint a window's background over region (image coords), ParentRelative
// borrowing its parent's
static bool paint_background(struct flame_xserver *xserver, struct xs_window *window,
		const struct target *t, const pixman_region32_t *region) {
	struct xs_window *owner = window;
	while (owner->background == XS_BACKGROUND_PARENT && owner->parent && owner->parent->parent) {
		owner = owner->parent;
	}
	struct xs_gc gc = {
		.function = GX_COPY,
		.plane_mask = ~0u,
		.foreground = owner->background_pixel,
	};
	if (owner->background == XS_BACKGROUND_PIXMAP) {
		int ox, oy, tx, ty;
		xs_window_origin(owner, &ox, &oy);
		xs_window_origin(t->top, &tx, &ty);
		gc.fill_style = FILL_TILED;
		gc.tile = owner->background_pixmap;
		// tiles start at the owner's origin, paint adds the target's offset
		gc.ts_x = (int16_t)(ox - tx - t->dx);
		gc.ts_y = (int16_t)(oy - ty - t->dy);
	} else if (owner->background != XS_BACKGROUND_PIXEL) {
		return false; // None: whatever was there stays
	}
	paint(xserver, t, &gc, region);
	return true;
}

static void send_expose(struct flame_xserver *xserver, struct xs_window *window, int x, int y,
		int w, int h, int count) {
	struct xs_event event = {0};
	xs_event_put8(&event, 0, XS_EVENT_EXPOSE);
	xs_event_put32(&event, 4, window->id);
	xs_event_put16(&event, 8, (uint16_t)x);
	xs_event_put16(&event, 10, (uint16_t)y);
	xs_event_put16(&event, 12, (uint16_t)w);
	xs_event_put16(&event, 14, (uint16_t)h);
	xs_event_put16(&event, 16, (uint16_t)count);
	xs_event_deliver(xserver, window, XS_MASK_EXPOSURE, &event);
}

// part of a window (window coords) shows anew: background, then Expose
static void expose_area(struct flame_xserver *xserver, struct xs_window *window, int x, int y,
		int w, int h) {
	struct target t;
	if (target_window(window, false, &t)) {
		pixman_region32_t region;
		pixman_region32_init_rect(&region, t.dx + x, t.dy + y, (unsigned)w, (unsigned)h);
		pixman_region32_intersect(&region, &region, &t.clip);
		if (paint_background(xserver, window, &t, &region)) {
			t.top->painted = true; // a real color or tile is something to show
		}
		damage_quiet(xserver, t.top, &region);
		pixman_region32_fini(&region);
		target_finish(&t);
	}
	if (xs_window_viewable(window) && window->class == 1) {
		send_expose(xserver, window, x, y, w, h, 0);
	}
}

static void expose_tree(struct flame_xserver *xserver, struct xs_window *window) {
	if (!xs_window_viewable(window) || window->class != 1) {
		return;
	}
	expose_area(xserver, window, 0, 0, window->width, window->height);
	struct xs_window *child;
	wl_list_for_each(child, &window->children, link) {
		expose_tree(xserver, child);
	}
}

// a top-level's backing at its size, what it showed kept where it fits. no
// screen is wider than MAX_BACKING, a window that is draws clipped there
// instead of taking gigabytes
#define MAX_BACKING 8192
static void backing_fit(struct xs_window *top) {
	if (top->class != 1) {
		return;
	}
	int w = top->width < MAX_BACKING ? top->width : MAX_BACKING;
	int h = top->height < MAX_BACKING ? top->height : MAX_BACKING;
	if (top->backing && pixman_image_get_width(top->backing) == w
			&& pixman_image_get_height(top->backing) == h) {
		return;
	}
	pixman_image_t *image = xs_image_create(top->depth == 32 ? 32 : 24, w, h, NULL, 0);
	if (!image) {
		wlr_log(WLR_ERROR, "xserver: no memory for a %dx%d window", w, h);
		return;
	}
	if (top->backing) {
		pixman_image_composite32(PIXMAN_OP_SRC, top->backing, NULL, image, 0, 0, 0, 0, 0, 0,
			w, h);
		pixman_image_unref(top->backing);
	}
	top->backing = image;
}

void xs_draw_window_shown(struct flame_xserver *xserver, struct xs_window *window) {
	if (!xs_window_viewable(window)) {
		return;
	}
	if (window->parent && !window->parent->parent) {
		backing_fit(window);
	}
	expose_tree(xserver, window);
}

void xs_draw_window_hidden(struct flame_xserver *xserver, struct xs_window *window) {
	if (window->parent && !window->parent->parent) {
		// a top-level: its pixels go, it's drawn again when it shows. the
		// screen there shows what's under it now
		xs_damage_root(xserver, window->x, window->y, window->width + 2 * window->border,
			window->height + 2 * window->border);
		if (window->backing) {
			pixman_image_unref(window->backing);
			window->backing = NULL;
		}
		window->damaged = false;
		window->painted = false;
		pixman_region32_clear(&window->damage);
		return;
	}
	// a child: where it was, its parent shows again
	struct xs_window *parent = window->parent;
	if (parent && xs_window_viewable(parent)) {
		expose_area(xserver, parent, window->x, window->y,
			window->width + 2 * window->border, window->height + 2 * window->border);
	}
}

void xs_draw_window_moved(struct flame_xserver *xserver, struct xs_window *window, int old_x,
		int old_y, int old_w, int old_h) {
	if (!xs_window_viewable(window)) {
		return;
	}
	if (window->parent && !window->parent->parent) {
		// a top-level: the screen changed where it was and where it is. where
		// it is is flame's business, only a new size redraws
		int b = 2 * window->border;
		xs_damage_root(xserver, old_x, old_y, old_w + b, old_h + b);
		xs_damage_root(xserver, window->x, window->y, window->width + b, window->height + b);
		if (window->width == old_w && window->height == old_h && window->backing) {
			return;
		}
		backing_fit(window);
	} else if (window->parent) {
		int b = 2 * window->border;
		expose_area(xserver, window->parent, old_x, old_y, old_w + b, old_h + b);
	}
	expose_tree(xserver, window);
}

void xs_draw_window_freed(struct xs_window *window) {
	if (window->backing) {
		pixman_image_unref(window->backing);
		window->backing = NULL;
	}
	pixman_region32_fini(&window->damage);
}

// ---- GCs ----

static void gc_set_clip_mask(struct flame_xserver *xserver, struct xs_gc *gc, uint32_t id) {
	pixman_region32_fini(&gc->clip);
	pixman_image_t *mask = gc_pixmap(xserver, id);
	gc->clipped = mask && pixman_image_get_format(mask) == PIXMAN_a1;
	if (gc->clipped) {
		pixman_region32_init_from_image(&gc->clip, mask);
	} else {
		pixman_region32_init(&gc->clip); // None: everything
	}
}

static void gc_change(struct xs_client *client, struct xs_gc *gc, uint32_t mask,
		const struct xin *in, size_t at) {
	for (int bit = 0; bit < 23; bit++) {
		if (!(mask & (1u << bit))) {
			continue;
		}
		uint32_t v = xin32(in, at);
		at += 4;
		switch (bit) {
		case 0:
			gc->function = (uint8_t)(v & 15);
			break;
		case 1:
			gc->plane_mask = v;
			break;
		case 2:
			gc->foreground = v;
			break;
		case 3:
			gc->background = v;
			break;
		case 4:
			gc->line_width = (uint16_t)v;
			break;
		case 5:
			gc->line_style = (uint8_t)v;
			break;
		case 6:
			gc->cap_style = (uint8_t)v;
			break;
		case 7:
			gc->join_style = (uint8_t)v;
			break;
		case 8:
			gc->fill_style = (uint8_t)v;
			break;
		case 9:
			gc->fill_rule = (uint8_t)v;
			break;
		case 10:
			gc->tile = v;
			break;
		case 11:
			gc->stipple = v;
			break;
		case 12:
			gc->ts_x = (int16_t)v;
			break;
		case 13:
			gc->ts_y = (int16_t)v;
			break;
		case 14:
			gc->font = v;
			break;
		case 15:
			gc->subwindow_mode = (uint8_t)v;
			break;
		case 16:
			gc->graphics_exposures = v & 1;
			break;
		case 17:
			gc->clip_x = (int16_t)v;
			break;
		case 18:
			gc->clip_y = (int16_t)v;
			break;
		case 19:
			gc_set_clip_mask(client->xserver, gc, v);
			break;
		case 22:
			gc->arc_mode = (uint8_t)v;
			break;
		default:
			break; // dashes: lines are solid
		}
	}
}

static void create_gc(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	uint32_t id = xin32(in, 4);
	if (!xs_id_available(client, id)) {
		xs_error(client, XS_ERROR_ID_CHOICE, id, 55, 0);
		return;
	}
	struct target t;
	bool missing;
	uint8_t depth = 24;
	if (target_get(client, xin32(in, 8), NULL, &t, &missing)) {
		depth = (uint8_t)t.depth;
		target_finish(&t);
	} else if (missing) {
		xs_error(client, XS_ERROR_DRAWABLE, xin32(in, 8), 55, 0);
		return;
	} else {
		struct xs_window *window = xserver_window(xserver, xin32(in, 8));
		struct xs_pixmap *pixmap = xs_pixmap_find(xserver, xin32(in, 8));
		depth = window ? window->depth : pixmap ? pixmap->depth : 24;
	}
	struct xs_gc *gc = calloc(1, sizeof(*gc));
	if (!gc) {
		xs_error(client, XS_ERROR_ALLOC, id, 55, 0);
		return;
	}
	*gc = (struct xs_gc){
		.id = id,
		.owner = client,
		.depth = depth,
		.function = GX_COPY,
		.plane_mask = ~0u,
		.background = 1,
		.cap_style = 1,
		.arc_mode = 1,
		.graphics_exposures = true,
	};
	pixman_region32_init(&gc->clip);
	wl_list_insert(&xserver->gcs, &gc->link);
	gc_change(client, gc, xin32(in, 12), in, 16);
}

static void copy_gc(struct xs_client *client, const struct xin *in) {
	struct xs_gc *src = get_gc(client, xin32(in, 4), 57, 0);
	struct xs_gc *dst = src ? get_gc(client, xin32(in, 8), 57, 0) : NULL;
	if (!dst) {
		return;
	}
	uint32_t mask = xin32(in, 12);
	if (mask & (1u << 0)) dst->function = src->function;
	if (mask & (1u << 1)) dst->plane_mask = src->plane_mask;
	if (mask & (1u << 2)) dst->foreground = src->foreground;
	if (mask & (1u << 3)) dst->background = src->background;
	if (mask & (1u << 4)) dst->line_width = src->line_width;
	if (mask & (1u << 5)) dst->line_style = src->line_style;
	if (mask & (1u << 6)) dst->cap_style = src->cap_style;
	if (mask & (1u << 7)) dst->join_style = src->join_style;
	if (mask & (1u << 8)) dst->fill_style = src->fill_style;
	if (mask & (1u << 9)) dst->fill_rule = src->fill_rule;
	if (mask & (1u << 10)) dst->tile = src->tile;
	if (mask & (1u << 11)) dst->stipple = src->stipple;
	if (mask & (1u << 12)) dst->ts_x = src->ts_x;
	if (mask & (1u << 13)) dst->ts_y = src->ts_y;
	if (mask & (1u << 14)) dst->font = src->font;
	if (mask & (1u << 15)) dst->subwindow_mode = src->subwindow_mode;
	if (mask & (1u << 16)) dst->graphics_exposures = src->graphics_exposures;
	if (mask & (1u << 17)) dst->clip_x = src->clip_x;
	if (mask & (1u << 18)) dst->clip_y = src->clip_y;
	if (mask & (1u << 19)) {
		pixman_region32_copy(&dst->clip, &src->clip);
		dst->clipped = src->clipped;
	}
	if (mask & (1u << 22)) dst->arc_mode = src->arc_mode;
}

static void set_clip_rectangles(struct xs_client *client, const struct xin *in) {
	struct xs_gc *gc = get_gc(client, xin32(in, 4), 59, 0);
	if (!gc) {
		return;
	}
	gc->clip_x = xin16s(in, 8);
	gc->clip_y = xin16s(in, 10);
	struct boxes b = {0};
	for (size_t at = 12; at + 8 <= in->len; at += 8) {
		int x = xin16s(in, at), y = xin16s(in, at + 2);
		boxes_add(&b, x, y, x + xin16(in, at + 4), y + xin16(in, at + 6));
	}
	pixman_region32_fini(&gc->clip);
	pixman_region32_init_rects(&gc->clip, b.data, (int)b.len);
	free(b.data);
	gc->clipped = true; // no rectangles: nothing gets drawn
}

void xs_gc_destroy(struct xs_gc *gc) {
	wl_list_remove(&gc->link);
	pixman_region32_fini(&gc->clip);
	free(gc);
}

// ---- drawing requests ----

static void no_expose(struct xs_client *client, const struct xs_gc *gc, uint32_t drawable,
		uint8_t major) {
	if (!gc->graphics_exposures) {
		return;
	}
	// nothing was hidden that needs drawing again: NoExpose
	struct xs_event event = {0};
	xs_event_put8(&event, 0, 14);
	xs_event_put32(&event, 4, drawable);
	xs_event_put16(&event, 8, 0);
	xs_event_put8(&event, 10, major);
	xs_event_send(client, &event);
}

static void poly_fill_rectangle(struct xs_client *client, const struct xin *in) {
	struct xs_gc *gc = get_gc(client, xin32(in, 8), 70, 0);
	struct target t;
	if (!gc || !target_for(client, xin32(in, 4), gc, &t, 70, 0)) {
		return;
	}
	struct boxes b = {0};
	for (size_t at = 12; at + 8 <= in->len; at += 8) {
		int x = xin16s(in, at), y = xin16s(in, at + 2);
		boxes_add(&b, x, y, x + xin16(in, at + 4), y + xin16(in, at + 6));
	}
	draw_boxes(client, &t, gc, &b);
	target_finish(&t);
}

static void poly_rectangle(struct xs_client *client, const struct xin *in) {
	struct xs_gc *gc = get_gc(client, xin32(in, 8), 67, 0);
	struct target t;
	if (!gc || !target_for(client, xin32(in, 4), gc, &t, 67, 0)) {
		return;
	}
	struct boxes b = {0};
	for (size_t at = 12; at + 8 <= in->len; at += 8) {
		int x = xin16s(in, at), y = xin16s(in, at + 2);
		int x2 = x + xin16(in, at + 4), y2 = y + xin16(in, at + 6);
		line(&b, gc, x, y, x2, y);
		line(&b, gc, x2, y, x2, y2);
		line(&b, gc, x2, y2, x, y2);
		line(&b, gc, x, y2, x, y);
	}
	draw_boxes(client, &t, gc, &b);
	target_finish(&t);
}

// PolyPoint and PolyLine: mode 0 absolute, 1 each relative to the one before
static void poly_points(struct xs_client *client, const struct xin *in, uint8_t opcode) {
	struct xs_gc *gc = get_gc(client, xin32(in, 8), opcode, 0);
	struct target t;
	if (!gc || !target_for(client, xin32(in, 4), gc, &t, opcode, 0)) {
		return;
	}
	bool relative = xin8(in, 1) == 1;
	struct boxes b = {0};
	int px = 0, py = 0;
	for (size_t at = 12, i = 0; at + 4 <= in->len; at += 4, i++) {
		int x = xin16s(in, at), y = xin16s(in, at + 2);
		if (relative && i > 0) {
			x += px;
			y += py;
		}
		if (opcode == 64) {
			boxes_add(&b, x, y, x + 1, y + 1);
		} else if (i > 0) {
			line(&b, gc, px, py, x, y);
		}
		px = x;
		py = y;
	}
	draw_boxes(client, &t, gc, &b);
	target_finish(&t);
}

static void poly_segment(struct xs_client *client, const struct xin *in) {
	struct xs_gc *gc = get_gc(client, xin32(in, 8), 66, 0);
	struct target t;
	if (!gc || !target_for(client, xin32(in, 4), gc, &t, 66, 0)) {
		return;
	}
	struct boxes b = {0};
	for (size_t at = 12; at + 8 <= in->len; at += 8) {
		line(&b, gc, xin16s(in, at), xin16s(in, at + 2), xin16s(in, at + 4), xin16s(in, at + 6));
	}
	draw_boxes(client, &t, gc, &b);
	target_finish(&t);
}

static void fill_poly(struct xs_client *client, const struct xin *in) {
	struct xs_gc *gc = get_gc(client, xin32(in, 8), 69, 0);
	struct target t;
	if (!gc || !target_for(client, xin32(in, 4), gc, &t, 69, 0)) {
		return;
	}
	bool relative = xin8(in, 13) == 1;
	size_t n = in->len >= 16 ? (in->len - 16) / 4 : 0;
	struct fpoint *p = malloc((n ? n : 1) * sizeof(*p));
	if (!p) {
		target_finish(&t);
		return;
	}
	int px = 0, py = 0;
	for (size_t i = 0; i < n; i++) {
		int x = xin16s(in, 16 + i * 4), y = xin16s(in, 18 + i * 4);
		if (relative && i > 0) {
			x += px;
			y += py;
		}
		p[i] = (struct fpoint){x, y};
		px = x;
		py = y;
	}
	struct boxes b = {0};
	polygon_fill(&b, p, n, gc->fill_rule == 1);
	free(p);
	draw_boxes(client, &t, gc, &b);
	target_finish(&t);
}

#define ARC_POINTS 722

static void poly_arc(struct xs_client *client, const struct xin *in, bool fill) {
	uint8_t opcode = fill ? 71 : 68;
	struct xs_gc *gc = get_gc(client, xin32(in, 8), opcode, 0);
	struct target t;
	if (!gc || !target_for(client, xin32(in, 4), gc, &t, opcode, 0)) {
		return;
	}
	struct fpoint *p = malloc((ARC_POINTS + 1) * sizeof(*p));
	struct boxes b = {0};
	for (size_t at = 12; p && at + 12 <= in->len; at += 12) {
		int x = xin16s(in, at), y = xin16s(in, at + 2);
		int w = xin16(in, at + 4), h = xin16(in, at + 6);
		int a1 = xin16s(in, at + 8), a2 = xin16s(in, at + 10);
		size_t n = arc_points(x, y, w, h, a1, a2, p, ARC_POINTS);
		bool full = a2 >= 360 * 64 || a2 <= -360 * 64;
		if (fill) {
			if (!full && gc->arc_mode == 1) {
				p[n++] = (struct fpoint){x + w / 2.0, y + h / 2.0}; // pie slice
			}
			polygon_fill(&b, p, n, true);
		} else {
			for (size_t i = 0; i + 1 < n; i++) {
				line(&b, gc, (int)lround(p[i].x), (int)lround(p[i].y), (int)lround(p[i + 1].x),
					(int)lround(p[i + 1].y));
			}
		}
	}
	free(p);
	draw_boxes(client, &t, gc, &b);
	target_finish(&t);
}

// an image's pixels into a drawable. format: 0 XYBitmap (1 bit per pixel,
// foreground and background), 1 XYPixmap, 2 ZPixmap. data is width x height
// (plus left_pad bits for the XY ones) with stride bytes a row; src_x,
// src_y, w, h pick a part of it
void xs_draw_put_image(struct xs_client *client, uint32_t drawable, uint32_t gc_id,
		uint8_t format, uint8_t depth, int width, int height, int left_pad, int src_x,
		int src_y, int w, int h, int dst_x, int dst_y, const uint8_t *data, size_t stride,
		size_t len, uint8_t major, uint16_t minor) {
	struct xs_gc *gc = get_gc(client, gc_id, major, minor);
	struct target t;
	if (!gc || !target_for(client, drawable, gc, &t, major, minor)) {
		return;
	}
	// XYBitmap: 1 bit per pixel through foreground and background.
	// XYPixmap: plane by plane, only 1 plane deep is ever sent (then it's the
	// pixels themselves). ZPixmap: the drawable's own pixels
	bool bitmap = format == 0 || format == 1;
	uint8_t error = 0;
	if (format == 0 ? depth != 1 : depth != t.depth) {
		error = XS_ERROR_MATCH;
	} else if (stride * (size_t)height > len) {
		error = XS_ERROR_LENGTH;
	}
	if (error || width <= 0 || height <= 0 || (format == 1 && depth != 1)) {
		if (error) {
			xs_error(client, error, 0, major, minor);
		}
		target_finish(&t);
		return;
	}
	pixman_image_t *src = xs_image_create(bitmap ? 1 : depth, width + (bitmap ? left_pad : 0),
		height, (void *)data, stride);
	if (!src) {
		target_finish(&t);
		return;
	}
	pixman_region32_t region;
	pixman_region32_init_rect(&region, t.dx + dst_x, t.dy + dst_y, (unsigned)w, (unsigned)h);
	pixman_region32_intersect(&region, &region, &t.clip);
	int sx = src_x + (bitmap ? left_pad : 0);
	if (format == 0 && t.depth != 1) {
		// the bitmap's 1s in foreground, 0s in background
		fill_solid(&t, gc, gc->background, GX_COPY, &region);
		pixman_color_t color = pixel_color(gc->foreground, t.depth);
		pixman_image_t *fg = pixman_image_create_solid_fill(&color);
		pixman_image_set_clip_region32(t.image, &region);
		pixman_image_composite32(PIXMAN_OP_OVER, fg, src, t.image, 0, 0, sx, src_y,
			t.dx + dst_x, t.dy + dst_y, w, h);
		pixman_image_set_clip_region32(t.image, NULL);
		pixman_image_unref(fg);
	} else if (format == 0) {
		// bitmap into a bitmap: the bits through foreground/background
		int n;
		const pixman_box32_t *box = pixman_region32_rectangles(&region, &n);
		for (int i = 0; i < n; i++) {
			for (int y = box[i].y1; y < box[i].y2; y++) {
				for (int x = box[i].x1; x < box[i].x2; x++) {
					uint32_t bit = read_pixel(src, x - t.dx - dst_x + sx, y - t.dy - dst_y + src_y);
					write_pixel(t.image, x, y, (bit ? gc->foreground : gc->background) & 1);
				}
			}
		}
	} else {
		pixman_image_set_clip_region32(t.image, &region);
		pixman_image_composite32(PIXMAN_OP_SRC, src, NULL, t.image, sx, src_y,
			0, 0, t.dx + dst_x, t.dy + dst_y, w, h);
		pixman_image_set_clip_region32(t.image, NULL);
	}
	pixman_image_unref(src);
	damage_target(client->xserver, &t, &region);
	pixman_region32_fini(&region);
	target_finish(&t);
}

static void put_image(struct xs_client *client, const struct xin *in) {
	uint8_t format = xin8(in, 1), depth = xin8(in, 21);
	int width = xin16(in, 12), height = xin16(in, 14), left_pad = xin8(in, 20);
	size_t stride = format == 2 ? xs_image_stride(depth, width)
		: xs_image_stride(1, width + left_pad);
	xs_draw_put_image(client, xin32(in, 4), xin32(in, 8), format, depth, width, height, left_pad,
		0, 0, width, height, xin16s(in, 16), xin16s(in, 18), in->data + 24, stride,
		in->len > 24 ? in->len - 24 : 0, 72, 0);
}

// a drawable's pixels as ZPixmap rows into out (stride bytes a row). false
// with the error sent
bool xs_draw_get_image(struct xs_client *client, uint32_t drawable, int x, int y, int w, int h,
		uint8_t *out, size_t stride, uint8_t *depth, uint32_t *visual, uint8_t major,
		uint16_t minor) {
	struct target t;
	bool missing;
	struct xs_window *window = xserver_window(client->xserver, drawable);
	if (!target_get(client, drawable, NULL, &t, &missing)) {
		if (missing) {
			xs_error(client, XS_ERROR_DRAWABLE, drawable, major, minor);
			return false;
		}
		// nothing to read (unmapped, a gpu buffer): black, like before
		struct xs_pixmap *pixmap = window ? NULL : xs_pixmap_find(client->xserver, drawable);
		*depth = window ? window->depth : pixmap ? pixmap->depth : 24;
		*visual = window ? window->visual : 0;
		memset(out, 0, stride * (size_t)h);
		return true;
	}
	if (x < 0 || y < 0 || x + w > t.width || y + h > t.height) {
		xs_error(client, XS_ERROR_MATCH, drawable, major, minor);
		target_finish(&t);
		return false;
	}
	*depth = (uint8_t)t.depth;
	*visual = window ? window->visual : 0;
	pixman_image_t *dst = xs_image_create(t.depth, w, h, out, stride);
	if (dst) {
		pixman_image_composite32(PIXMAN_OP_SRC, t.image, NULL, dst, t.dx + x, t.dy + y, 0, 0,
			0, 0, w, h);
		pixman_image_unref(dst);
	}
	target_finish(&t);
	return true;
}

static void get_image(struct xs_client *client, const struct xin *in) {
	int w = xin16(in, 12), h = xin16(in, 14);
	struct xs_window *window = xserver_window(client->xserver, xin32(in, 4));
	struct xs_pixmap *pixmap = window ? NULL : xs_pixmap_find(client->xserver, xin32(in, 4));
	int d = window ? window->depth : pixmap ? pixmap->depth : 24;
	size_t stride = xs_image_stride(d, w);
	if (stride * (size_t)h > (16u << 20)) {
		xs_error(client, XS_ERROR_VALUE, 0, 73, 0);
		return;
	}
	uint8_t *pixels = calloc(1, stride * (size_t)h + 4);
	if (!pixels) {
		xs_error(client, XS_ERROR_ALLOC, 0, 73, 0);
		return;
	}
	uint8_t depth;
	uint32_t visual;
	if (xs_draw_get_image(client, xin32(in, 4), xin16s(in, 8), xin16s(in, 10), w, h, pixels,
			stride, &depth, &visual, 73, 0)) {
		struct xout out;
		xs_reply_begin(client, &out, depth);
		xout32(&out, visual);
		xout_zero(&out, 20);
		xout_bytes(&out, pixels, stride * (size_t)h);
		xs_reply_send(client, &out);
	}
	free(pixels);
}

// the source drawable's pixels for CopyArea and CopyPlane: its image and
// where its 0,0 is in it
static bool source_get(struct xs_client *client, uint32_t id, pixman_image_t **image,
		int *dx, int *dy, int *width, int *height, int *depth, uint8_t major) {
	struct target t;
	bool missing;
	if (!target_get(client, id, NULL, &t, &missing)) {
		if (missing) {
			xs_error(client, XS_ERROR_DRAWABLE, id, major, 0);
		}
		return false;
	}
	*image = t.image;
	*dx = t.dx;
	*dy = t.dy;
	*width = t.width;
	*height = t.height;
	*depth = t.depth;
	target_finish(&t);
	return true;
}

// the part of a source rectangle inside the source drawable, moved to match
static bool copy_rect(int src_w, int src_h, int *sx, int *sy, int *dx, int *dy, int *w, int *h) {
	if (*sx < 0) {
		*dx -= *sx;
		*w += *sx;
		*sx = 0;
	}
	if (*sy < 0) {
		*dy -= *sy;
		*h += *sy;
		*sy = 0;
	}
	if (*sx + *w > src_w) {
		*w = src_w - *sx;
	}
	if (*sy + *h > src_h) {
		*h = src_h - *sy;
	}
	return *w > 0 && *h > 0;
}

static void copy_area(struct xs_client *client, const struct xin *in) {
	uint32_t src_id = xin32(in, 4), dst_id = xin32(in, 8);
	struct xs_gc *gc = get_gc(client, xin32(in, 12), 62, 0);
	if (!gc) {
		return;
	}
	pixman_image_t *src;
	int sdx, sdy, sw, sh, sdepth;
	struct target t;
	bool have_src = source_get(client, src_id, &src, &sdx, &sdy, &sw, &sh, &sdepth, 62);
	if (!have_src || !target_for(client, dst_id, gc, &t, 62, 0)) {
		no_expose(client, gc, dst_id, 62);
		return;
	}
	if (sdepth != t.depth) {
		xs_error(client, XS_ERROR_MATCH, dst_id, 62, 0);
		target_finish(&t);
		return;
	}
	int sx = xin16s(in, 16), sy = xin16s(in, 18), dx = xin16s(in, 20), dy = xin16s(in, 22);
	int w = xin16(in, 24), h = xin16(in, 26);
	if (copy_rect(sw, sh, &sx, &sy, &dx, &dy, &w, &h)) {
		pixman_region32_t region;
		pixman_region32_init_rect(&region, t.dx + dx, t.dy + dy, (unsigned)w, (unsigned)h);
		pixman_region32_intersect(&region, &region, &t.clip);
		pixman_image_t *from = src;
		int fx = sdx + sx, fy = sdy + sy;
		if (src == t.image) {
			// overlapping within one image (scrolling): through a copy
			from = xs_image_create(t.depth, w, h, NULL, 0);
			if (from) {
				pixman_image_composite32(PIXMAN_OP_SRC, src, NULL, from, fx, fy, 0, 0, 0, 0, w, h);
			}
			fx = fy = 0;
		}
		if (from) {
			pixman_image_set_clip_region32(t.image, &region);
			pixman_image_composite32(PIXMAN_OP_SRC, from, NULL, t.image, fx, fy, 0, 0,
				t.dx + dx, t.dy + dy, w, h);
			pixman_image_set_clip_region32(t.image, NULL);
			if (from != src) {
				pixman_image_unref(from);
			}
		}
		damage_target(client->xserver, &t, &region);
		pixman_region32_fini(&region);
	}
	target_finish(&t);
	no_expose(client, gc, dst_id, 62);
}

// one bit plane of the source through foreground/background
static void copy_plane(struct xs_client *client, const struct xin *in) {
	uint32_t src_id = xin32(in, 4), dst_id = xin32(in, 8);
	struct xs_gc *gc = get_gc(client, xin32(in, 12), 63, 0);
	if (!gc) {
		return;
	}
	pixman_image_t *src;
	int sdx, sdy, sw, sh, sdepth;
	struct target t;
	if (!source_get(client, src_id, &src, &sdx, &sdy, &sw, &sh, &sdepth, 63)
			|| !target_for(client, dst_id, gc, &t, 63, 0)) {
		no_expose(client, gc, dst_id, 63);
		return;
	}
	int sx = xin16s(in, 16), sy = xin16s(in, 18), dx = xin16s(in, 20), dy = xin16s(in, 22);
	int w = xin16(in, 24), h = xin16(in, 26);
	uint32_t plane = xin32(in, 28);
	if (copy_rect(sw, sh, &sx, &sy, &dx, &dy, &w, &h)) {
		// the plane as a bitmap
		pixman_image_t *mask = xs_image_create(1, w, h, NULL, 0);
		for (int y = 0; mask && y < h; y++) {
			for (int x = 0; x < w; x++) {
				uint32_t v = read_pixel(src, sdx + sx + x, sdy + sy + y);
				write_pixel(mask, x, y, sdepth == 1 ? v : (v & plane) != 0);
			}
		}
		pixman_region32_t region;
		pixman_region32_init_rect(&region, t.dx + dx, t.dy + dy, (unsigned)w, (unsigned)h);
		pixman_region32_intersect(&region, &region, &t.clip);
		if (mask && t.depth != 1) {
			fill_solid(&t, gc, gc->background, GX_COPY, &region);
			pixman_color_t color = pixel_color(gc->foreground, t.depth);
			pixman_image_t *fg = pixman_image_create_solid_fill(&color);
			pixman_image_set_clip_region32(t.image, &region);
			pixman_image_composite32(PIXMAN_OP_OVER, fg, mask, t.image, 0, 0, 0, 0,
				t.dx + dx, t.dy + dy, w, h);
			pixman_image_set_clip_region32(t.image, NULL);
			pixman_image_unref(fg);
		} else if (mask) {
			for (int y = 0; y < h; y++) {
				for (int x = 0; x < w; x++) {
					if (pixman_region32_contains_point(&region, t.dx + dx + x, t.dy + dy + y, NULL)) {
						write_pixel(t.image, t.dx + dx + x, t.dy + dy + y,
							(read_pixel(mask, x, y) ? gc->foreground : gc->background) & 1);
					}
				}
			}
		}
		if (mask) {
			pixman_image_unref(mask);
		}
		damage_target(client->xserver, &t, &region);
		pixman_region32_fini(&region);
	}
	target_finish(&t);
	no_expose(client, gc, dst_id, 63);
}

static void clear_area(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	struct xs_window *window = xserver_window(xserver, xin32(in, 4));
	if (!window) {
		xs_error(client, XS_ERROR_WINDOW, xin32(in, 4), 61, 0);
		return;
	}
	int x = xin16s(in, 8), y = xin16s(in, 10), w = xin16(in, 12), h = xin16(in, 14);
	if (w == 0) {
		w = window->width - x;
	}
	if (h == 0) {
		h = window->height - y;
	}
	if (w <= 0 || h <= 0) {
		return;
	}
	struct target t;
	if (target_window(window, false, &t)) {
		pixman_region32_t region;
		pixman_region32_init_rect(&region, t.dx + x, t.dy + y, (unsigned)w, (unsigned)h);
		pixman_region32_intersect(&region, &region, &t.clip);
		if (paint_background(xserver, window, &t, &region)) {
			t.top->painted = true; // a real color or tile is something to show
		}
		damage_quiet(xserver, t.top, &region);
		pixman_region32_fini(&region);
		target_finish(&t);
	}
	if (xin8(in, 1) && xs_window_viewable(window)) {
		send_expose(xserver, window, x, y, w, h, 0);
	}
}

// ---- pixmaps ----

#define MAX_PIXMAP_BYTES (256u << 20)

bool xs_draw_create_pixmap(struct xs_client *client, uint32_t id, int depth, int width,
		int height, void *bits, size_t stride, struct xs_shm_segment *shm, uint8_t major,
		uint16_t minor) {
	struct flame_xserver *xserver = client->xserver;
	if (!xs_id_available(client, id)) {
		xs_error(client, XS_ERROR_ID_CHOICE, id, major, minor);
		return false;
	}
	if (!depth_format(depth) || width <= 0 || height <= 0) {
		xs_error(client, XS_ERROR_VALUE, (uint32_t)depth, major, minor);
		return false;
	}
	// nobody needs a picture this big, and a client asking shouldn't get
	// to take flame's memory: Xorg says BadAlloc too
	if (!bits && xs_image_stride(depth, width) * (size_t)height > MAX_PIXMAP_BYTES) {
		xs_error(client, XS_ERROR_ALLOC, id, major, minor);
		return false;
	}
	struct xs_pixmap *pixmap = calloc(1, sizeof(*pixmap));
	pixman_image_t *image = pixmap ? xs_image_create(depth, width, height, bits, stride) : NULL;
	if (!image) {
		free(pixmap);
		xs_error(client, XS_ERROR_ALLOC, id, major, minor);
		return false;
	}
	pixmap->id = id;
	pixmap->owner = client;
	pixmap->depth = (uint8_t)depth;
	pixmap->bpp = (uint8_t)depth_bpp(depth);
	pixmap->width = (uint16_t)width;
	pixmap->height = (uint16_t)height;
	pixmap->image = image;
	pixmap->shm = shm;
	if (shm) {
		shm->refs++;
	}
	wl_list_insert(&xserver->pixmaps, &pixmap->link);
	return true;
}

// ---- dispatch ----

bool xs_draw_request(struct xs_client *client, uint8_t opcode, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	switch (opcode) {
	case 53: // CreatePixmap
		xs_draw_create_pixmap(client, xin32(in, 4), xin8(in, 1), xin16(in, 12), xin16(in, 14),
			NULL, 0, NULL, 53, 0);
		return true;
	case 55:
		create_gc(client, in);
		return true;
	case 56: { // ChangeGC
		struct xs_gc *gc = get_gc(client, xin32(in, 4), 56, 0);
		if (gc) {
			gc_change(client, gc, xin32(in, 8), in, 12);
		}
		return true;
	}
	case 57:
		copy_gc(client, in);
		return true;
	case 58: // SetDashes: lines are solid
		return true;
	case 59:
		set_clip_rectangles(client, in);
		return true;
	case 60: { // FreeGC
		struct xs_gc *gc = get_gc(client, xin32(in, 4), 60, 0);
		if (gc) {
			xs_gc_destroy(gc);
		}
		return true;
	}
	case 61:
		clear_area(client, in);
		return true;
	case 62:
		copy_area(client, in);
		return true;
	case 63:
		copy_plane(client, in);
		return true;
	case 64: // PolyPoint
	case 65: // PolyLine
		poly_points(client, in, opcode);
		return true;
	case 66:
		poly_segment(client, in);
		return true;
	case 67:
		poly_rectangle(client, in);
		return true;
	case 68:
		poly_arc(client, in, false);
		return true;
	case 69:
		fill_poly(client, in);
		return true;
	case 70:
		poly_fill_rectangle(client, in);
		return true;
	case 71:
		poly_arc(client, in, true);
		return true;
	case 72:
		put_image(client, in);
		return true;
	case 73:
		get_image(client, in);
		return true;
	default:
		(void)xserver;
		return false;
	}
}
