#include <stdlib.h>
#include <string.h>
#include <wlr/util/log.h>

#include "xserver/internal.h"

// RENDER 0.11 on pixman, like Xorg's fb does it: pictures are pixman images
// over a drawable's pixels (a window's view into its top-level's backing,
// clipped to what of it shows), solid fills and gradients are pixman's own,
// and Composite, trapezoids, triangles and glyphs are pixman calls. cairo,
// Xft, Qt and friends draw almost everything through it. without it cairo
// falls back to paths that put text into double buffers too late (Xorg's
// Xvfb with RENDER off shows the same)

#define RENDER_MINOR 11

enum {
	ERROR_FORMAT,
	ERROR_PICTURE,
	ERROR_OP,
	ERROR_GLYPHSET,
	ERROR_GLYPH,
};

#define FORMAT_A8R8G8B8 0x30
#define FORMAT_X8R8G8B8 0x31
#define FORMAT_A8 0x32
#define FORMAT_A1 0x33

struct format {
	uint32_t id;
	uint8_t depth;
	pixman_format_code_t code;
	// shift and mask per channel: red, green, blue, alpha
	uint16_t channels[8];
};

static const struct format formats[] = {
	{FORMAT_A8R8G8B8, 32, PIXMAN_a8r8g8b8, {16, 0xff, 8, 0xff, 0, 0xff, 24, 0xff}},
	{FORMAT_X8R8G8B8, 24, PIXMAN_x8r8g8b8, {16, 0xff, 8, 0xff, 0, 0xff, 0, 0}},
	{FORMAT_A8, 8, PIXMAN_a8, {0, 0, 0, 0, 0, 0, 0, 0xff}},
	{FORMAT_A1, 1, PIXMAN_a1, {0, 0, 0, 0, 0, 0, 0, 1}},
};

static const struct format *find_format(uint32_t id) {
	for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
		if (formats[i].id == id) {
			return &formats[i];
		}
	}
	return NULL;
}

struct picture {
	struct wl_list link; // flame_xserver.pictures
	uint32_t id;
	struct xs_client *owner;
	uint32_t drawable; // 0: a fill or gradient, source is its image
	const struct format *format;
	pixman_image_t *source;
	uint8_t repeat;
	bool component_alpha;
	uint8_t subwindow_mode;
	int16_t clip_x, clip_y;
	bool clipped; // rectangles or a bitmap, drawable coords before the origin
	pixman_region32_t clip;
	bool transformed;
	pixman_transform_t transform;
	pixman_filter_t filter;
	pixman_fixed_t *filter_params;
	int filter_n;
};

struct glyph {
	uint32_t id;
	pixman_image_t *image;
	int16_t x, y, x_off, y_off;
};

// glyphs sorted by id. several ids can share it (ReferenceGlyphSet)
struct glyphset {
	const struct format *format;
	struct glyph *glyphs;
	size_t len, cap;
	int refs;
};

struct glyphset_id {
	struct wl_list link; // flame_xserver.glyphset_ids
	uint32_t id;
	struct xs_client *owner;
	struct glyphset *set;
};

static void error(struct xs_client *client, int code, uint32_t value, uint8_t minor) {
	xs_error(client, (uint8_t)(XS_RENDER_FIRST_ERROR + code), value, XS_RENDER_OPCODE, minor);
}

// ---- pictures ----

static struct picture *picture_find(struct flame_xserver *xserver, uint32_t id) {
	struct picture *picture;
	wl_list_for_each(picture, &xserver->pictures, link) {
		if (picture->id == id) {
			return picture;
		}
	}
	return NULL;
}

static struct picture *get_picture(struct xs_client *client, uint32_t id, uint8_t minor) {
	struct picture *picture = picture_find(client->xserver, id);
	if (!picture) {
		error(client, ERROR_PICTURE, id, minor);
	}
	return picture;
}

static void picture_free(struct picture *picture) {
	wl_list_remove(&picture->link);
	if (picture->source) {
		pixman_image_unref(picture->source);
	}
	pixman_region32_fini(&picture->clip);
	free(picture->filter_params);
	free(picture);
}

static struct picture *picture_new(struct xs_client *client, uint32_t id, uint8_t minor) {
	if (!xs_id_available(client, id)) {
		xs_error(client, XS_ERROR_ID_CHOICE, id, XS_RENDER_OPCODE, minor);
		return NULL;
	}
	struct picture *picture = calloc(1, sizeof(*picture));
	if (!picture) {
		xs_error(client, XS_ERROR_ALLOC, id, XS_RENDER_OPCODE, minor);
		return NULL;
	}
	picture->id = id;
	picture->owner = client;
	picture->filter = PIXMAN_FILTER_NEAREST;
	pixman_region32_init(&picture->clip);
	wl_list_insert(&client->xserver->pictures, &picture->link);
	return picture;
}

static void set_clip_mask(struct flame_xserver *xserver, struct picture *picture, uint32_t id) {
	pixman_region32_fini(&picture->clip);
	struct xs_pixmap *mask = id ? xs_pixmap_find(xserver, id) : NULL;
	picture->clipped = mask && mask->image && pixman_image_get_format(mask->image) == PIXMAN_a1;
	if (picture->clipped) {
		pixman_region32_init_from_image(&picture->clip, mask->image);
	} else {
		pixman_region32_init(&picture->clip);
	}
}

static void change_picture(struct xs_client *client, struct picture *picture, uint32_t mask,
		const struct xin *in, size_t at) {
	for (int bit = 0; bit < 13; bit++) {
		if (!(mask & (1u << bit))) {
			continue;
		}
		uint32_t v = xin32(in, at);
		at += 4;
		switch (bit) {
		case 0:
			picture->repeat = (uint8_t)(v & 3);
			break;
		case 4:
			picture->clip_x = (int16_t)v;
			break;
		case 5:
			picture->clip_y = (int16_t)v;
			break;
		case 6:
			set_clip_mask(client->xserver, picture, v);
			break;
		case 8:
			picture->subwindow_mode = (uint8_t)v;
			break;
		case 12:
			picture->component_alpha = v & 1;
			break;
		default:
			break; // alpha maps, graphics exposures, poly edges, dither
		}
	}
}

// a picture's pixels as pixman wants them, coords the drawable's own
struct view {
	pixman_image_t *image;
	struct xs_window *top; // windows: where damage goes
	int dx, dy; // the drawable in the backing
	pixman_region32_t shows; // drawable coords, what may be drawn on
};

static void view_finish(struct view *v) {
	if (v->image) {
		pixman_image_unref(v->image);
	}
	pixman_region32_fini(&v->shows);
}

static bool view_get(struct flame_xserver *xserver, struct picture *picture, struct view *v) {
	*v = (struct view){0};
	pixman_region32_init(&v->shows);
	if (!picture->drawable) {
		v->image = pixman_image_ref(picture->source);
	} else {
		struct xs_window *window = xserver_window(xserver, picture->drawable);
		pixman_image_t *pixels;
		int w, h;
		if (window) {
			pixman_region32_t clip;
			pixman_region32_init(&clip);
			if (!xs_window_pixels(window, picture->subwindow_mode == 1, &pixels, &v->dx, &v->dy,
					&clip)) {
				pixman_region32_fini(&clip);
				return false;
			}
			v->top = xs_window_top(window);
			w = window->width;
			h = window->height;
			// the view can't start before the backing does, nor run past it
			int bw = pixman_image_get_width(pixels), bh = pixman_image_get_height(pixels);
			if (v->dx < 0 || v->dy < 0 || v->dx >= bw || v->dy >= bh) {
				pixman_region32_fini(&clip);
				return false;
			}
			w = w < bw - v->dx ? w : bw - v->dx;
			h = h < bh - v->dy ? h : bh - v->dy;
			pixman_region32_translate(&clip, -v->dx, -v->dy);
			pixman_region32_copy(&v->shows, &clip);
			pixman_region32_fini(&clip);
		} else {
			struct xs_pixmap *pixmap = xs_pixmap_find(xserver, picture->drawable);
			if (!pixmap || !pixmap->image) {
				return false;
			}
			pixels = pixmap->image;
			w = pixmap->width;
			h = pixmap->height;
			pixman_region32_init_rect(&v->shows, 0, 0, (unsigned)w, (unsigned)h);
		}
		if (PIXMAN_FORMAT_BPP(picture->format->code)
				!= PIXMAN_FORMAT_BPP(pixman_image_get_format(pixels))) {
			return false;
		}
		int stride = pixman_image_get_stride(pixels);
		uint8_t *bits = (uint8_t *)pixman_image_get_data(pixels) + (size_t)v->dy * (size_t)stride
			+ (size_t)v->dx * (size_t)(PIXMAN_FORMAT_BPP(picture->format->code) / 8);
		v->image = pixman_image_create_bits(picture->format->code, w, h, (uint32_t *)bits, stride);
		if (!v->image) {
			return false;
		}
	}
	if (picture->clipped) {
		pixman_region32_t clip;
		pixman_region32_init(&clip);
		pixman_region32_copy(&clip, &picture->clip);
		pixman_region32_translate(&clip, picture->clip_x, picture->clip_y);
		pixman_region32_intersect(&v->shows, &v->shows, &clip);
		pixman_region32_fini(&clip);
	}
	pixman_image_set_repeat(v->image, (pixman_repeat_t)picture->repeat);
	pixman_image_set_component_alpha(v->image, picture->component_alpha);
	pixman_image_set_transform(v->image, picture->transformed ? &picture->transform : NULL);
	pixman_image_set_filter(v->image, picture->filter, picture->filter_params,
		picture->filter_n);
	return true;
}

// a destination: clipped to what shows
static bool dest_get(struct flame_xserver *xserver, struct picture *picture, struct view *v) {
	if (!picture->drawable) {
		*v = (struct view){0};
		pixman_region32_init(&v->shows);
		return false;
	}
	if (!view_get(xserver, picture, v)) {
		return false;
	}
	pixman_image_set_clip_region32(v->image, &v->shows);
	return true;
}

// pixels changed in box (drawable coords): flame hears about windows
static void dest_damage(struct flame_xserver *xserver, struct view *v, int x1, int y1, int x2,
		int y2) {
	if (!v->top || x2 <= x1 || y2 <= y1) {
		return;
	}
	pixman_region32_t region;
	pixman_region32_init_rect(&region, x1, y1, (unsigned)(x2 - x1), (unsigned)(y2 - y1));
	pixman_region32_intersect(&region, &region, &v->shows);
	pixman_region32_translate(&region, v->dx, v->dy);
	xs_damage(xserver, v->top, &region);
	pixman_region32_fini(&region);
}

static bool valid_op(uint8_t op) {
	return op <= 0x0d || (op >= 0x10 && op <= 0x1b) || (op >= 0x20 && op <= 0x2b)
		|| (op >= 0x30 && op <= 0x3e);
}

// the src (and mask) of an operation. NULL id: no mask
static bool source_get(struct xs_client *client, uint32_t id, struct view *v, uint8_t minor,
		bool *ok) {
	*ok = true;
	if (!id) {
		*v = (struct view){0};
		pixman_region32_init(&v->shows);
		return false;
	}
	struct picture *picture = get_picture(client, id, minor);
	if (!picture) {
		*ok = false;
		return false;
	}
	if (!view_get(client->xserver, picture, v)) {
		*ok = false; // nothing to read (unmapped, a gpu pixmap): nothing drawn
		return false;
	}
	return true;
}

static void create_picture(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	uint32_t drawable = xin32(in, 8);
	const struct format *format = find_format(xin32(in, 12));
	if (!format) {
		error(client, ERROR_FORMAT, xin32(in, 12), 4);
		return;
	}
	struct xs_window *window = xserver_window(xserver, drawable);
	struct xs_pixmap *pixmap = window ? NULL : xs_pixmap_find(xserver, drawable);
	if (!window && !pixmap) {
		xs_error(client, XS_ERROR_DRAWABLE, drawable, XS_RENDER_OPCODE, 4);
		return;
	}
	int depth = window ? window->depth : pixmap->depth;
	if (depth != format->depth) {
		xs_error(client, XS_ERROR_MATCH, drawable, XS_RENDER_OPCODE, 4);
		return;
	}
	struct picture *picture = picture_new(client, xin32(in, 4), 4);
	if (!picture) {
		return;
	}
	picture->drawable = drawable;
	picture->format = format;
	change_picture(client, picture, xin32(in, 16), in, 20);
}

static void set_clip_rectangles(struct xs_client *client, const struct xin *in) {
	struct picture *picture = get_picture(client, xin32(in, 4), 6);
	if (!picture) {
		return;
	}
	picture->clip_x = xin16s(in, 8);
	picture->clip_y = xin16s(in, 10);
	size_t n = in->len > 12 ? (in->len - 12) / 8 : 0;
	pixman_box32_t *boxes = calloc(n ? n : 1, sizeof(*boxes));
	if (!boxes) {
		return;
	}
	for (size_t i = 0; i < n; i++) {
		int x = xin16s(in, 12 + i * 8), y = xin16s(in, 14 + i * 8);
		boxes[i] = (pixman_box32_t){x, y, x + xin16(in, 16 + i * 8), y + xin16(in, 18 + i * 8)};
	}
	pixman_region32_fini(&picture->clip);
	pixman_region32_init_rects(&picture->clip, boxes, (int)n);
	picture->clipped = true;
	free(boxes);
}

static void set_transform(struct xs_client *client, const struct xin *in) {
	struct picture *picture = get_picture(client, xin32(in, 4), 28);
	if (!picture) {
		return;
	}
	for (int i = 0; i < 9; i++) {
		picture->transform.matrix[i / 3][i % 3] = (pixman_fixed_t)xin32(in, 8 + (size_t)i * 4);
	}
	pixman_transform_t identity;
	pixman_transform_init_identity(&identity);
	picture->transformed = memcmp(&identity, &picture->transform, sizeof(identity)) != 0;
}

static const char *const filter_names[] = {
	"nearest", "bilinear", "convolution", "separable-convolution", "fast", "good", "best",
};

static void set_filter(struct xs_client *client, const struct xin *in) {
	struct picture *picture = get_picture(client, xin32(in, 4), 30);
	if (!picture) {
		return;
	}
	size_t len = xin16(in, 8);
	if (12 + len > in->len) {
		xs_error(client, XS_ERROR_LENGTH, 0, XS_RENDER_OPCODE, 30);
		return;
	}
	char name[32] = {0};
	memcpy(name, in->data + 12, len < sizeof(name) - 1 ? len : sizeof(name) - 1);
	static const pixman_filter_t map[] = {
		PIXMAN_FILTER_NEAREST, PIXMAN_FILTER_BILINEAR, PIXMAN_FILTER_CONVOLUTION,
		PIXMAN_FILTER_SEPARABLE_CONVOLUTION, PIXMAN_FILTER_NEAREST, PIXMAN_FILTER_BILINEAR,
		PIXMAN_FILTER_BILINEAR,
	};
	size_t which = sizeof(map) / sizeof(map[0]);
	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
		if (!strcmp(name, filter_names[i])) {
			which = i;
		}
	}
	if (which == sizeof(map) / sizeof(map[0])) {
		xs_error(client, XS_ERROR_NAME, 0, XS_RENDER_OPCODE, 30);
		return;
	}
	size_t at = 12 + ((len + 3) & ~(size_t)3);
	int n = at < in->len ? (int)((in->len - at) / 4) : 0;
	free(picture->filter_params);
	picture->filter_params = n ? calloc((size_t)n, sizeof(pixman_fixed_t)) : NULL;
	picture->filter_n = picture->filter_params ? n : 0;
	for (int i = 0; i < picture->filter_n; i++) {
		picture->filter_params[i] = (pixman_fixed_t)xin32(in, at + (size_t)i * 4);
	}
	picture->filter = map[which];
}

static void query_filters(struct xs_client *client) {
	size_t count = sizeof(filter_names) / sizeof(filter_names[0]);
	// fast is nearest, good and best are bilinear
	static const uint16_t aliases[] = {0xffff, 0xffff, 0xffff, 0xffff, 0, 1, 1};
	struct xout out;
	xs_reply_begin(client, &out, 0);
	xout32(&out, (uint32_t)count);
	xout32(&out, (uint32_t)count);
	xout_zero(&out, 16);
	for (size_t i = 0; i < count; i++) {
		xout16(&out, aliases[i]);
	}
	xout_pad(&out);
	for (size_t i = 0; i < count; i++) {
		xout8(&out, (uint8_t)strlen(filter_names[i]));
		xout_bytes(&out, filter_names[i], strlen(filter_names[i]));
	}
	xout_pad(&out);
	xs_reply_send(client, &out);
}

// ---- fills and gradients ----

static pixman_color_t read_color(const struct xin *in, size_t at) {
	return (pixman_color_t){xin16(in, at), xin16(in, at + 2), xin16(in, at + 4),
		xin16(in, at + 6)};
}

static void create_solid(struct xs_client *client, const struct xin *in) {
	struct picture *picture = picture_new(client, xin32(in, 4), 33);
	if (!picture) {
		return;
	}
	pixman_color_t color = read_color(in, 8);
	picture->source = pixman_image_create_solid_fill(&color);
	picture->format = &formats[0];
	if (!picture->source) {
		picture_free(picture);
		xs_error(client, XS_ERROR_ALLOC, xin32(in, 4), XS_RENDER_OPCODE, 33);
	}
}

static pixman_gradient_stop_t *read_stops(const struct xin *in, size_t at, uint32_t n) {
	if (n == 0 || n > 4096 || at + (size_t)n * 12 > in->len) {
		return NULL;
	}
	pixman_gradient_stop_t *stops = calloc(n, sizeof(*stops));
	for (uint32_t i = 0; stops && i < n; i++) {
		stops[i].x = (pixman_fixed_t)xin32(in, at + (size_t)i * 4);
		stops[i].color = read_color(in, at + (size_t)n * 4 + (size_t)i * 8);
	}
	return stops;
}

static pixman_point_fixed_t read_point(const struct xin *in, size_t at) {
	return (pixman_point_fixed_t){(pixman_fixed_t)xin32(in, at),
		(pixman_fixed_t)xin32(in, at + 4)};
}

static void create_gradient(struct xs_client *client, const struct xin *in, uint8_t minor) {
	uint32_t n;
	size_t at;
	switch (minor) {
	case 34:
		n = xin32(in, 24);
		at = 28;
		break;
	case 35:
		n = xin32(in, 32);
		at = 36;
		break;
	default:
		n = xin32(in, 20);
		at = 24;
		break;
	}
	pixman_gradient_stop_t *stops = read_stops(in, at, n);
	if (!stops) {
		xs_error(client, XS_ERROR_LENGTH, 0, XS_RENDER_OPCODE, minor);
		return;
	}
	struct picture *picture = picture_new(client, xin32(in, 4), minor);
	if (!picture) {
		free(stops);
		return;
	}
	picture->format = &formats[0];
	if (minor == 34) {
		pixman_point_fixed_t p1 = read_point(in, 8), p2 = read_point(in, 16);
		picture->source = pixman_image_create_linear_gradient(&p1, &p2, stops, (int)n);
	} else if (minor == 35) {
		pixman_point_fixed_t inner = read_point(in, 8), outer = read_point(in, 16);
		picture->source = pixman_image_create_radial_gradient(&inner, &outer,
			(pixman_fixed_t)xin32(in, 24), (pixman_fixed_t)xin32(in, 28), stops, (int)n);
	} else {
		pixman_point_fixed_t center = read_point(in, 8);
		picture->source = pixman_image_create_conical_gradient(&center,
			(pixman_fixed_t)xin32(in, 16), stops, (int)n);
	}
	free(stops);
	if (!picture->source) {
		picture_free(picture);
		xs_error(client, XS_ERROR_ALLOC, xin32(in, 4), XS_RENDER_OPCODE, minor);
	}
}

// ---- compositing ----

static void composite(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	uint8_t op = xin8(in, 4);
	if (!valid_op(op)) {
		error(client, ERROR_OP, op, 8);
		return;
	}
	struct picture *dst_pic = get_picture(client, xin32(in, 16), 8);
	if (!dst_pic) {
		return;
	}
	struct view src = {0}, mask = {0}, dst = {0};
	bool ok;
	if (!source_get(client, xin32(in, 8), &src, 8, &ok)) {
		view_finish(&src);
		return;
	}
	bool has_mask = source_get(client, xin32(in, 12), &mask, 8, &ok);
	if (!ok) {
		view_finish(&src);
		view_finish(&mask);
		return;
	}
	if (dest_get(xserver, dst_pic, &dst)) {
		int dx = xin16s(in, 28), dy = xin16s(in, 30), w = xin16(in, 32), h = xin16(in, 34);
		pixman_image_composite32((pixman_op_t)op, src.image, has_mask ? mask.image : NULL,
			dst.image, xin16s(in, 20), xin16s(in, 22), xin16s(in, 24), xin16s(in, 26), dx, dy,
			w, h);
		dest_damage(xserver, &dst, dx, dy, dx + w, dy + h);
	}
	view_finish(&dst);
	view_finish(&src);
	view_finish(&mask);
}

static void fill_rectangles(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	uint8_t op = xin8(in, 4);
	if (!valid_op(op)) {
		error(client, ERROR_OP, op, 26);
		return;
	}
	struct picture *picture = get_picture(client, xin32(in, 8), 26);
	struct view dst = {0};
	if (!picture || !dest_get(xserver, picture, &dst)) {
		view_finish(&dst);
		return;
	}
	pixman_color_t color = read_color(in, 12);
	size_t n = in->len > 20 ? (in->len - 20) / 8 : 0;
	pixman_rectangle16_t *rects = calloc(n ? n : 1, sizeof(*rects));
	if (rects) {
		int x1 = 0x7fffffff, y1 = 0x7fffffff, x2 = -0x7fffffff, y2 = -0x7fffffff;
		for (size_t i = 0; i < n; i++) {
			rects[i] = (pixman_rectangle16_t){xin16s(in, 20 + i * 8), xin16s(in, 22 + i * 8),
				xin16(in, 24 + i * 8), xin16(in, 26 + i * 8)};
			x1 = rects[i].x < x1 ? rects[i].x : x1;
			y1 = rects[i].y < y1 ? rects[i].y : y1;
			x2 = rects[i].x + rects[i].width > x2 ? rects[i].x + rects[i].width : x2;
			y2 = rects[i].y + rects[i].height > y2 ? rects[i].y + rects[i].height : y2;
		}
		pixman_image_fill_rectangles((pixman_op_t)op, dst.image, &color, (int)n, rects);
		dest_damage(xserver, &dst, x1, y1, x2, y2);
		free(rects);
	}
	view_finish(&dst);
}

// the box around fixed point coords, whole pixels
static void grow(int *box, pixman_fixed_t x, pixman_fixed_t y) {
	int fx = pixman_fixed_to_int(x), cx = pixman_fixed_to_int(pixman_fixed_ceil(x));
	int fy = pixman_fixed_to_int(y), cy = pixman_fixed_to_int(pixman_fixed_ceil(y));
	box[0] = fx < box[0] ? fx : box[0];
	box[1] = fy < box[1] ? fy : box[1];
	box[2] = cx > box[2] ? cx : box[2];
	box[3] = cy > box[3] ? cy : box[3];
}

// Trapezoids, Triangles, TriStrip, TriFan: shapes into the destination
// through the source, antialiased in mask_format
static void shapes(struct xs_client *client, const struct xin *in, uint8_t minor) {
	struct flame_xserver *xserver = client->xserver;
	uint8_t op = xin8(in, 4);
	if (!valid_op(op)) {
		error(client, ERROR_OP, op, minor);
		return;
	}
	const struct format *mask_format = xin32(in, 16) ? find_format(xin32(in, 16)) : NULL;
	if (xin32(in, 16) && !mask_format) {
		error(client, ERROR_FORMAT, xin32(in, 16), minor);
		return;
	}
	struct picture *dst_pic = get_picture(client, xin32(in, 12), minor);
	if (!dst_pic) {
		return;
	}
	struct view src = {0}, dst = {0};
	bool ok;
	if (!source_get(client, xin32(in, 8), &src, minor, &ok)) {
		view_finish(&src);
		return;
	}
	if (!dest_get(xserver, dst_pic, &dst)) {
		view_finish(&dst);
		view_finish(&src);
		return;
	}
	// without a mask format pixman still wants one: a8 is what Xorg uses
	pixman_format_code_t format = mask_format ? mask_format->code : PIXMAN_a8;
	int sx = xin16s(in, 20), sy = xin16s(in, 22);
	int box[4] = {0x7fffffff, 0x7fffffff, -0x7fffffff, -0x7fffffff};
	size_t body = in->len > 24 ? in->len - 24 : 0;
	const uint8_t *data = in->data + 24;
	if (minor == 10) {
		size_t n = body / sizeof(pixman_trapezoid_t);
		const pixman_trapezoid_t *traps = (const pixman_trapezoid_t *)data;
		for (size_t i = 0; i < n; i++) {
			grow(box, traps[i].left.p1.x < traps[i].left.p2.x ? traps[i].left.p1.x
				: traps[i].left.p2.x, traps[i].top);
			grow(box, traps[i].right.p1.x > traps[i].right.p2.x ? traps[i].right.p1.x
				: traps[i].right.p2.x, traps[i].bottom);
		}
		pixman_composite_trapezoids((pixman_op_t)op, src.image, dst.image, format, sx, sy, 0, 0,
			(int)n, traps);
	} else {
		size_t n = 0;
		pixman_triangle_t *tris = NULL;
		if (minor == 11) {
			n = body / sizeof(pixman_triangle_t);
			tris = calloc(n ? n : 1, sizeof(*tris));
			if (tris) {
				memcpy(tris, data, n * sizeof(*tris));
			}
		} else {
			size_t points = body / sizeof(pixman_point_fixed_t);
			const pixman_point_fixed_t *p = (const pixman_point_fixed_t *)data;
			n = points >= 3 ? points - 2 : 0;
			tris = calloc(n ? n : 1, sizeof(*tris));
			for (size_t i = 0; tris && i < n; i++) {
				// strips: each new point with the two before. fans: around the first
				tris[i] = minor == 12
					? (pixman_triangle_t){p[i], p[i + 1], p[i + 2]}
					: (pixman_triangle_t){p[0], p[i + 1], p[i + 2]};
			}
		}
		for (size_t i = 0; tris && i < n; i++) {
			grow(box, tris[i].p1.x, tris[i].p1.y);
			grow(box, tris[i].p2.x, tris[i].p2.y);
			grow(box, tris[i].p3.x, tris[i].p3.y);
		}
		if (tris) {
			pixman_composite_triangles((pixman_op_t)op, src.image, dst.image, format, sx, sy, 0,
				0, (int)n, tris);
			free(tris);
		}
	}
	dest_damage(xserver, &dst, box[0], box[1], box[2], box[3]);
	view_finish(&dst);
	view_finish(&src);
}

static void add_traps(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	struct picture *picture = get_picture(client, xin32(in, 4), 32);
	struct view dst = {0};
	if (!picture || !dest_get(xserver, picture, &dst)) {
		view_finish(&dst);
		return;
	}
	size_t n = in->len > 12 ? (in->len - 12) / sizeof(pixman_trap_t) : 0;
	const pixman_trap_t *traps = (const pixman_trap_t *)(in->data + 12);
	int x = xin16s(in, 8), y = xin16s(in, 10);
	pixman_add_traps(dst.image, (int16_t)x, (int16_t)y, (int)n, traps);
	int box[4] = {0x7fffffff, 0x7fffffff, -0x7fffffff, -0x7fffffff};
	for (size_t i = 0; i < n; i++) {
		grow(box, traps[i].top.l, traps[i].top.y);
		grow(box, traps[i].bot.r, traps[i].bot.y);
		grow(box, traps[i].bot.l, traps[i].top.y);
		grow(box, traps[i].top.r, traps[i].bot.y);
	}
	dest_damage(xserver, &dst, box[0] + x, box[1] + y, box[2] + x, box[3] + y);
	view_finish(&dst);
}

// ---- glyphs ----

static struct glyphset_id *glyphset_find(struct flame_xserver *xserver, uint32_t id) {
	struct glyphset_id *gs;
	wl_list_for_each(gs, &xserver->glyphset_ids, link) {
		if (gs->id == id) {
			return gs;
		}
	}
	return NULL;
}

static struct glyphset *get_glyphset(struct xs_client *client, uint32_t id, uint8_t minor) {
	struct glyphset_id *gs = glyphset_find(client->xserver, id);
	if (!gs) {
		error(client, ERROR_GLYPHSET, id, minor);
		return NULL;
	}
	return gs->set;
}

// pixman's glyph cache keys: the set and the glyph's id. the glyphs move
// around in the set's array as it grows, their addresses can't be keys
#define GLYPH_KEY(id) ((void *)(uintptr_t)(id))

static void glyph_forget(struct flame_xserver *xserver, struct glyphset *set,
		struct glyph *glyph) {
	if (xserver->glyph_cache) {
		pixman_glyph_cache_remove(xserver->glyph_cache, set, GLYPH_KEY(glyph->id));
	}
	pixman_image_unref(glyph->image);
}

static void glyphset_unref(struct flame_xserver *xserver, struct glyphset *set) {
	if (--set->refs > 0) {
		return;
	}
	for (size_t i = 0; i < set->len; i++) {
		glyph_forget(xserver, set, &set->glyphs[i]);
	}
	free(set->glyphs);
	free(set);
}

bool xs_render_id_in_use(struct flame_xserver *xserver, uint32_t id) {
	return picture_find(xserver, id) || glyphset_find(xserver, id);
}

size_t xs_render_ids(struct flame_xserver *xserver, uint32_t base, uint32_t *out, size_t max) {
	size_t len = 0;
	// out NULL: just how many there are
	struct picture *picture;
	wl_list_for_each(picture, &xserver->pictures, link) {
		if ((picture->id & ~0x1fffffu) == base && (!out || len < max)) {
			if (out) {
				out[len] = picture->id;
			}
			len++;
		}
	}
	struct glyphset_id *gs;
	wl_list_for_each(gs, &xserver->glyphset_ids, link) {
		if ((gs->id & ~0x1fffffu) == base && (!out || len < max)) {
			if (out) {
				out[len] = gs->id;
			}
			len++;
		}
	}
	return len;
}

static bool glyphset_id_new(struct xs_client *client, uint32_t id, struct glyphset *set,
		uint8_t minor) {
	if (!xs_id_available(client, id)) {
		xs_error(client, XS_ERROR_ID_CHOICE, id, XS_RENDER_OPCODE, minor);
		return false;
	}
	struct glyphset_id *gs = calloc(1, sizeof(*gs));
	if (!gs) {
		xs_error(client, XS_ERROR_ALLOC, id, XS_RENDER_OPCODE, minor);
		return false;
	}
	gs->id = id;
	gs->owner = client;
	gs->set = set;
	set->refs++;
	wl_list_insert(&client->xserver->glyphset_ids, &gs->link);
	return true;
}

static void glyphset_id_free(struct flame_xserver *xserver, struct glyphset_id *gs) {
	wl_list_remove(&gs->link);
	glyphset_unref(xserver, gs->set);
	free(gs);
}

static struct glyph *glyph_find(struct glyphset *set, uint32_t id, size_t *at) {
	size_t lo = 0, hi = set->len;
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		if (set->glyphs[mid].id < id) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	if (at) {
		*at = lo;
	}
	return lo < set->len && set->glyphs[lo].id == id ? &set->glyphs[lo] : NULL;
}

static void create_glyphset(struct xs_client *client, const struct xin *in) {
	const struct format *format = find_format(xin32(in, 8));
	if (!format) {
		error(client, ERROR_FORMAT, xin32(in, 8), 17);
		return;
	}
	struct glyphset *set = calloc(1, sizeof(*set));
	if (!set) {
		xs_error(client, XS_ERROR_ALLOC, xin32(in, 4), XS_RENDER_OPCODE, 17);
		return;
	}
	set->format = format;
	if (!glyphset_id_new(client, xin32(in, 4), set, 17)) {
		free(set);
	}
}

static void add_glyphs(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	struct glyphset *set = get_glyphset(client, xin32(in, 4), 20);
	if (!set) {
		return;
	}
	uint32_t n = xin32(in, 8);
	size_t ids = 12, infos = ids + (size_t)n * 4, images = infos + (size_t)n * 12;
	if (n > 65536 || images > in->len) {
		xs_error(client, XS_ERROR_LENGTH, 0, XS_RENDER_OPCODE, 20);
		return;
	}
	int bpp = PIXMAN_FORMAT_BPP(set->format->code);
	size_t at = images;
	for (uint32_t i = 0; i < n; i++) {
		size_t info = infos + (size_t)i * 12;
		int w = xin16(in, info), h = xin16(in, info + 2);
		size_t stride = ((size_t)w * (size_t)bpp + 31) / 32 * 4;
		size_t size = stride * (size_t)h;
		if (at + size > in->len) {
			xs_error(client, XS_ERROR_LENGTH, 0, XS_RENDER_OPCODE, 20);
			return;
		}
		pixman_image_t *image = NULL;
		if (w > 0 && h > 0) {
			image = pixman_image_create_bits(set->format->code, w, h, NULL, (int)stride);
			if (image) {
				memcpy(pixman_image_get_data(image), in->data + at, size);
			}
		} else {
			image = pixman_image_create_bits(set->format->code, 1, 1, NULL, 4); // empty: a space
		}
		at += size;
		if (!image) {
			xs_error(client, XS_ERROR_ALLOC, 0, XS_RENDER_OPCODE, 20);
			return;
		}
		uint32_t id = xin32(in, ids + (size_t)i * 4);
		size_t pos;
		struct glyph *glyph = glyph_find(set, id, &pos);
		if (glyph) {
			glyph_forget(xserver, set, glyph); // replaced
		} else {
			if (set->len == set->cap) {
				size_t cap = set->cap ? set->cap * 2 : 128;
				struct glyph *glyphs = realloc(set->glyphs, cap * sizeof(*glyphs));
				if (!glyphs) {
					pixman_image_unref(image);
					xs_error(client, XS_ERROR_ALLOC, 0, XS_RENDER_OPCODE, 20);
					return;
				}
				set->glyphs = glyphs;
				set->cap = cap;
			}
			memmove(&set->glyphs[pos + 1], &set->glyphs[pos],
				(set->len - pos) * sizeof(*set->glyphs));
			set->len++;
			glyph = &set->glyphs[pos];
		}
		*glyph = (struct glyph){
			.id = id,
			.image = image,
			.x = xin16s(in, info + 4),
			.y = xin16s(in, info + 6),
			.x_off = xin16s(in, info + 8),
			.y_off = xin16s(in, info + 10),
		};
	}
}

static void free_glyphs(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	struct glyphset *set = get_glyphset(client, xin32(in, 4), 22);
	for (size_t at = 8; set && at + 4 <= in->len; at += 4) {
		size_t pos;
		struct glyph *glyph = glyph_find(set, xin32(in, at), &pos);
		if (glyph) {
			glyph_forget(xserver, set, glyph);
			memmove(&set->glyphs[pos], &set->glyphs[pos + 1],
				(set->len - pos - 1) * sizeof(*set->glyphs));
			set->len--;
		}
	}
}

// CompositeGlyphs8/16/32: runs of glyphs, each run starting at a pen offset,
// a run of length 255 switches the glyph set. like Xorg's fbGlyphs: pixman
// lays them out, through a mask of mask_format or one by one without
static void composite_glyphs(struct xs_client *client, const struct xin *in, uint8_t minor) {
	struct flame_xserver *xserver = client->xserver;
	uint8_t op = xin8(in, 4);
	if (!valid_op(op)) {
		error(client, ERROR_OP, op, minor);
		return;
	}
	const struct format *mask_format = xin32(in, 16) ? find_format(xin32(in, 16)) : NULL;
	if (xin32(in, 16) && !mask_format) {
		error(client, ERROR_FORMAT, xin32(in, 16), minor);
		return;
	}
	struct glyphset *set = get_glyphset(client, xin32(in, 20), minor);
	struct picture *dst_pic = set ? get_picture(client, xin32(in, 12), minor) : NULL;
	if (!dst_pic) {
		return;
	}
	if (!xserver->glyph_cache) {
		xserver->glyph_cache = pixman_glyph_cache_create();
		if (!xserver->glyph_cache) {
			return;
		}
	}
	size_t size = minor == 23 ? 1 : minor == 24 ? 2 : 4;
	// how many glyphs at most: every byte past the header a glyph
	size_t max = in->len > 28 ? in->len - 28 : 0;
	pixman_glyph_t *glyphs = calloc(max ? max : 1, sizeof(*glyphs));
	if (!glyphs) {
		return;
	}
	pixman_glyph_cache_freeze(xserver->glyph_cache);
	size_t n = 0;
	int x = 0, y = 0, first_x = 0, first_y = 0;
	bool first = true;
	size_t at = 28;
	while (at + 8 <= in->len) {
		uint8_t len = xin8(in, at);
		int dx = xin16s(in, at + 4), dy = xin16s(in, at + 6);
		at += 8;
		if (len == 255) {
			// a glyph set switch, its id follows
			if (at + 4 > in->len) {
				break;
			}
			struct glyphset *next = get_glyphset(client, xin32(in, at), minor);
			at += 4;
			if (!next) {
				break;
			}
			set = next;
			continue;
		}
		x += dx;
		y += dy;
		if (first) {
			first_x = x;
			first_y = y;
			first = false;
		}
		for (uint8_t i = 0; i < len && at + size <= in->len; i++, at += size) {
			uint32_t id = size == 1 ? xin8(in, at) : size == 2 ? xin16(in, at) : xin32(in, at);
			struct glyph *glyph = glyph_find(set, id, NULL);
			if (!glyph) {
				continue;
			}
			const void *cached = pixman_glyph_cache_lookup(xserver->glyph_cache, set,
				GLYPH_KEY(id));
			if (!cached) {
				cached = pixman_glyph_cache_insert(xserver->glyph_cache, set, GLYPH_KEY(id), glyph->x,
					glyph->y, glyph->image);
			}
			if (cached && n < max) {
				glyphs[n++] = (pixman_glyph_t){x, y, cached};
			}
			x += glyph->x_off;
			y += glyph->y_off;
		}
		at = (at + 3) & ~(size_t)3; // runs are padded to 4 bytes
	}
	struct view src = {0}, dst = {0};
	bool ok;
	if (n > 0 && source_get(client, xin32(in, 8), &src, minor, &ok)) {
		if (dest_get(xserver, dst_pic, &dst)) {
			int sx = xin16s(in, 24), sy = xin16s(in, 26);
			pixman_box32_t e;
			pixman_glyph_get_extents(xserver->glyph_cache, (int)n, glyphs, &e);
			if (mask_format) {
				pixman_composite_glyphs((pixman_op_t)op, src.image, dst.image, mask_format->code,
					sx + e.x1 - first_x, sy + e.y1 - first_y, e.x1, e.y1, e.x1, e.y1, e.x2 - e.x1,
					e.y2 - e.y1, xserver->glyph_cache, (int)n, glyphs);
			} else {
				pixman_composite_glyphs_no_mask((pixman_op_t)op, src.image, dst.image,
					sx - first_x, sy - first_y, 0, 0, xserver->glyph_cache, (int)n, glyphs);
			}
			dest_damage(xserver, &dst, e.x1, e.y1, e.x2, e.y2);
		}
		view_finish(&dst);
	}
	view_finish(&src);
	pixman_glyph_cache_thaw(xserver->glyph_cache);
	free(glyphs);
}

// ---- formats ----

static void query_version(struct xs_client *client) {
	struct xout out;
	xs_reply_begin(client, &out, 0);
	xout32(&out, 0);
	xout32(&out, RENDER_MINOR);
	xout_zero(&out, 16);
	xs_reply_send(client, &out);
}

static void query_pict_formats(struct xs_client *client) {
	size_t count = sizeof(formats) / sizeof(formats[0]);
	struct xout out;
	xs_reply_begin(client, &out, 0);
	xout32(&out, (uint32_t)count);
	xout32(&out, 1); // screens
	xout32(&out, 5); // depths
	xout32(&out, 2); // visuals
	xout32(&out, 1); // subpixel orders
	xout32(&out, 0);
	for (size_t i = 0; i < count; i++) {
		xout32(&out, formats[i].id);
		xout8(&out, 1); // direct
		xout8(&out, formats[i].depth);
		xout16(&out, 0);
		for (int c = 0; c < 8; c++) {
			xout16(&out, formats[i].channels[c]);
		}
		xout32(&out, 0); // colormap
	}
	// the screen: its depths, the visuals in them and their formats
	xout32(&out, 5);
	xout32(&out, FORMAT_X8R8G8B8); // fallback
	static const struct {
		uint8_t depth;
		uint32_t visual, format;
	} depths[] = {
		{24, XS_VISUAL_24, FORMAT_X8R8G8B8},
		{32, XS_VISUAL_32, FORMAT_A8R8G8B8},
		{8, 0, 0},
		{4, 0, 0},
		{1, 0, 0},
	};
	for (size_t i = 0; i < 5; i++) {
		xout8(&out, depths[i].depth);
		xout8(&out, 0);
		xout16(&out, depths[i].visual ? 1 : 0);
		xout32(&out, 0);
		if (depths[i].visual) {
			xout32(&out, depths[i].visual);
			xout32(&out, depths[i].format);
		}
	}
	xout32(&out, 0); // subpixel: unknown
	xs_reply_send(client, &out);
}

// ---- the rest ----

void xs_render_request(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	uint8_t minor = xin8(in, 1);
	switch (minor) {
	case 0:
		query_version(client);
		break;
	case 1:
		query_pict_formats(client);
		break;
	case 2: // QueryPictIndexValues: nothing indexed here
		xs_error(client, XS_ERROR_MATCH, xin32(in, 4), XS_RENDER_OPCODE, 2);
		break;
	case 4:
		create_picture(client, in);
		break;
	case 5: {
		struct picture *picture = get_picture(client, xin32(in, 4), 5);
		if (picture) {
			change_picture(client, picture, xin32(in, 8), in, 12);
		}
		break;
	}
	case 6:
		set_clip_rectangles(client, in);
		break;
	case 7: {
		struct picture *picture = get_picture(client, xin32(in, 4), 7);
		if (picture) {
			picture_free(picture);
		}
		break;
	}
	case 8:
		composite(client, in);
		break;
	case 10: // Trapezoids
	case 11: // Triangles
	case 12: // TriStrip
	case 13: // TriFan
		shapes(client, in, minor);
		break;
	case 17:
		create_glyphset(client, in);
		break;
	case 18: { // ReferenceGlyphSet
		struct glyphset *set = get_glyphset(client, xin32(in, 8), 18);
		if (set) {
			glyphset_id_new(client, xin32(in, 4), set, 18);
		}
		break;
	}
	case 19: { // FreeGlyphSet
		struct glyphset_id *gs = glyphset_find(xserver, xin32(in, 4));
		if (!gs) {
			error(client, ERROR_GLYPHSET, xin32(in, 4), 19);
		} else {
			glyphset_id_free(xserver, gs);
		}
		break;
	}
	case 20:
		add_glyphs(client, in);
		break;
	case 22:
		free_glyphs(client, in);
		break;
	case 23:
	case 24:
	case 25:
		composite_glyphs(client, in, minor);
		break;
	case 26:
		fill_rectangles(client, in);
		break;
	case 27: // CreateCursor
	case 31: // CreateAnimCursor
		break; // flame draws the pointer, X cursors don't show
	case 28:
		set_transform(client, in);
		break;
	case 29:
		query_filters(client);
		break;
	case 30:
		set_filter(client, in);
		break;
	case 32:
		add_traps(client, in);
		break;
	case 33:
		create_solid(client, in);
		break;
	case 34:
	case 35:
	case 36:
		create_gradient(client, in, minor);
		break;
	case 9: // Scale, 14 ColorTrapezoids, 15 ColorTriangles, 16 Transform: never
	case 14: // implemented anywhere, 21 AddGlyphsFromPicture: obsolete
	case 15:
	case 16:
	case 21:
	default:
		xs_error(client, XS_ERROR_REQUEST, 0, XS_RENDER_OPCODE, minor);
		break;
	}
}

void xs_render_client_gone(struct flame_xserver *xserver, struct xs_client *client) {
	struct picture *picture, *ptmp;
	wl_list_for_each_safe(picture, ptmp, &xserver->pictures, link) {
		if (picture->owner == client) {
			picture_free(picture);
		}
	}
	struct glyphset_id *gs, *gtmp;
	wl_list_for_each_safe(gs, gtmp, &xserver->glyphset_ids, link) {
		if (gs->owner == client) {
			glyphset_id_free(xserver, gs);
		}
	}
}

void xs_render_finish(struct flame_xserver *xserver) {
	// the clients took theirs along, whatever's left goes too
	struct picture *picture, *ptmp;
	wl_list_for_each_safe(picture, ptmp, &xserver->pictures, link) {
		picture_free(picture);
	}
	struct glyphset_id *gs, *gtmp;
	wl_list_for_each_safe(gs, gtmp, &xserver->glyphset_ids, link) {
		glyphset_id_free(xserver, gs);
	}
	if (xserver->glyph_cache) {
		pixman_glyph_cache_destroy(xserver->glyph_cache);
		xserver->glyph_cache = NULL;
	}
}
