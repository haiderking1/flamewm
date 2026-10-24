#include <pixman.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/util/log.h>

#include "xserver/internal.h"

// cursors: what the pointer looks like over X's windows. core ones from two
// bitmaps and two colors or from a font's glyphs (XCreateFontCursor: the
// cursor font's, a theme cursor by name here), RENDER's from an ARGB
// picture (Xcursor: gtk, qt, wine), animated ones from those. windows and
// grabs show them, XFixes hides them, flame draws whichever applies where
// the pointer is (xserver_pointer_cursor)

// the cursor font's glyphs, two a shape (the shape, its mask), as theme
// names. themes today only have the css ones, so the legacy names map to
// those where one fits
static const char *const font_shapes[] = {
	"default", "default", "default", "default", "default", // X_cursor .. boat
	"not-allowed", "sw-resize", "se-resize", "s-resize", "default", // bogosity .. bottom_tee
	"default", "default", "not-allowed", "wait", "wait", // box_spiral .. coffee_mug
	"crosshair", "crosshair", "crosshair", "crosshair", "default", // cross .. dot
	"default", "ns-resize", "default", "default", "default", // dotbox .. draped_box
	"default", "move", "default", "default", "pointer", // exchange .. hand1
	"pointer", "default", "default", "default", "default", // hand2 .. left_ptr
	"w-resize", "default", "default", "default", "default", // left_side .. lr_angle
	"default", "default", "default", "default", "not-allowed", // man .. pirate
	"cell", "help", "default", "e-resize", "default", // plus .. right_tee
	"default", "default", "default", "ns-resize", "ew-resize", // rightbutton .. sb_h_double
	"ew-resize", "ew-resize", "ns-resize", "ns-resize", "default", // sb_left .. shuttle
	"nwse-resize", "default", "default", "default", "crosshair", // sizing .. target
	"crosshair", "default", "nw-resize", "ne-resize", "n-resize", // tcross .. top_side
	"default", "default", "default", "default", "default", // top_tee .. ur_angle
	"wait", "text", // watch, xterm
};

#define FONT_SHAPES (sizeof(font_shapes) / sizeof(font_shapes[0]))

static uint32_t serials;

struct xs_cursor *xs_cursor_find(struct flame_xserver *xserver, uint32_t id) {
	struct xs_cursor *cursor;
	wl_list_for_each(cursor, &xserver->cursors, link) {
		if (id && cursor->id == id) {
			return cursor;
		}
	}
	return NULL;
}

static struct xs_cursor *get_cursor(struct xs_client *client, uint32_t id, uint8_t major,
		uint8_t minor) {
	struct xs_cursor *cursor = xs_cursor_find(client->xserver, id);
	if (!cursor) {
		xs_error(client, XS_ERROR_CURSOR, id, major, minor);
	}
	return cursor;
}

struct xs_cursor *xs_cursor_ref(struct xs_cursor *cursor) {
	if (cursor) {
		cursor->refs++;
	}
	return cursor;
}

void xs_cursor_unref(struct xs_cursor *cursor) {
	if (!cursor || --cursor->refs > 0) {
		return;
	}
	wl_list_remove(&cursor->link);
	for (size_t i = 0; i < cursor->len; i++) {
		free(cursor->images[i].pixels);
	}
	free(cursor->images);
	free(cursor->source);
	free(cursor->mask);
	free(cursor);
}

bool xs_cursor_set(struct xs_client *client, struct xs_cursor **slot, uint32_t id,
		uint8_t major, uint8_t minor) {
	struct xs_cursor *cursor = NULL;
	if (id && !(cursor = get_cursor(client, id, major, minor))) {
		return false;
	}
	if (cursor != *slot) {
		xs_cursor_ref(cursor);
		xs_cursor_unref(*slot);
		*slot = cursor;
		xs_cursor_changed(client->xserver);
	}
	return true;
}

void xs_cursor_changed(struct flame_xserver *xserver) {
	if (xserver->hooks.cursor_changed) {
		xserver->hooks.cursor_changed(xserver->hooks.data);
	}
}

// a new one under id, its first reference the id's
static struct xs_cursor *cursor_new(struct xs_client *client, uint32_t id, uint8_t major,
		uint8_t minor) {
	if (!xs_id_available(client, id)) {
		xs_error(client, XS_ERROR_ID_CHOICE, id, major, minor);
		return NULL;
	}
	struct xs_cursor *cursor = calloc(1, sizeof(*cursor));
	if (!cursor) {
		xs_error(client, XS_ERROR_ALLOC, id, major, minor);
		return NULL;
	}
	cursor->id = id;
	cursor->owner = client;
	cursor->refs = 1;
	cursor->serial = ++serials;
	wl_list_insert(&client->xserver->cursors, &cursor->link);
	return cursor;
}

// the id goes, windows showing it keep it
static void cursor_free_id(struct xs_cursor *cursor) {
	cursor->id = 0;
	xs_cursor_unref(cursor);
}

static uint32_t rgb(const uint16_t color[3]) {
	return 0xff000000u | (uint32_t)(color[0] >> 8) << 16 | (uint32_t)(color[1] >> 8) << 8
		| (uint32_t)(color[2] >> 8);
}

// a bitmap cursor's pixels from its bits and colors: the source's 1s in
// the foreground, 0s in the background, only where the mask is
static bool paint_bits(struct xs_cursor *cursor) {
	struct xs_cursor_image *image = &cursor->images[0];
	uint32_t fore = rgb(cursor->fore), back = rgb(cursor->back);
	size_t n = (size_t)image->width * image->height;
	for (size_t i = 0; i < n; i++) {
		image->pixels[i] = !cursor->mask[i] ? 0 : cursor->source[i] ? fore : back;
	}
	return true;
}

// one image, width x height, its bits taken in when it works
static bool bits_cursor(struct xs_cursor *cursor, uint8_t *source, uint8_t *mask, int width,
		int height, int hot_x, int hot_y) {
	cursor->images = calloc(1, sizeof(*cursor->images));
	uint32_t *pixels = malloc((size_t)width * (size_t)height * 4);
	if (!cursor->images || !pixels) {
		free(pixels);
		return false; // the bits stay the caller's
	}
	cursor->len = 1;
	cursor->images[0] = (struct xs_cursor_image){
		.pixels = pixels,
		.width = (uint16_t)width,
		.height = (uint16_t)height,
		.hot_x = (uint16_t)hot_x,
		.hot_y = (uint16_t)hot_y,
	};
	cursor->source = source;
	cursor->mask = mask;
	return paint_bits(cursor);
}

// a depth 1 pixmap as a byte a pixel, 0 or 1. NULL pixmap: all 1s
static uint8_t *pixmap_bits(struct xs_pixmap *pixmap, int width, int height) {
	uint8_t *bits = malloc((size_t)width * (size_t)height);
	if (!bits) {
		return NULL;
	}
	if (!pixmap) {
		memset(bits, 1, (size_t)width * (size_t)height);
		return bits;
	}
	// pixman knows its bit order: through an a8 copy
	pixman_image_t *a8 = pixman_image_create_bits(PIXMAN_a8, width, height, NULL, 0);
	if (!a8) {
		free(bits);
		return NULL;
	}
	memset(pixman_image_get_data(a8), 0, (size_t)pixman_image_get_stride(a8) * (size_t)height);
	pixman_image_composite32(PIXMAN_OP_SRC, pixmap->image, NULL, a8, 0, 0, 0, 0, 0, 0,
		pixmap->width < width ? pixmap->width : width,
		pixmap->height < height ? pixmap->height : height);
	const uint8_t *data = (const uint8_t *)pixman_image_get_data(a8);
	int stride = pixman_image_get_stride(a8);
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			bits[(size_t)y * (size_t)width + (size_t)x] = data[(size_t)y * (size_t)stride
				+ (size_t)x] != 0;
		}
	}
	pixman_image_unref(a8);
	return bits;
}

static void colors(const struct xin *in, size_t at, uint16_t fore[3], uint16_t back[3]) {
	for (int i = 0; i < 3; i++) {
		fore[i] = xin16(in, at + 2 * (size_t)i);
		back[i] = xin16(in, at + 6 + 2 * (size_t)i);
	}
}

// CreateCursor: id, source, mask, fore rgb, back rgb, hotspot
static void create_cursor(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	uint32_t source_id = xin32(in, 8), mask_id = xin32(in, 12);
	struct xs_pixmap *source = xs_pixmap_find(xserver, source_id);
	struct xs_pixmap *mask = mask_id ? xs_pixmap_find(xserver, mask_id) : NULL;
	if (!source || (mask_id && !mask)) {
		xs_error(client, XS_ERROR_PIXMAP, source ? mask_id : source_id, 93, 0);
		return;
	}
	int hot_x = xin16(in, 28), hot_y = xin16(in, 30);
	if (source->depth != 1 || !source->image || (mask && (mask->depth != 1 || !mask->image
			|| mask->width != source->width || mask->height != source->height))
			|| hot_x >= source->width || hot_y >= source->height) {
		xs_error(client, XS_ERROR_MATCH, source_id, 93, 0);
		return;
	}
	struct xs_cursor *cursor = cursor_new(client, xin32(in, 4), 93, 0);
	if (!cursor) {
		return;
	}
	colors(in, 16, cursor->fore, cursor->back);
	int w = source->width, h = source->height;
	uint8_t *source_bits = pixmap_bits(source, w, h), *mask_bits = pixmap_bits(mask, w, h);
	if (!source_bits || !mask_bits || !bits_cursor(cursor, source_bits, mask_bits, w, h, hot_x,
			hot_y)) {
		if (!cursor->source) {
			free(source_bits);
			free(mask_bits);
		}
		cursor_free_id(cursor);
		xs_error(client, XS_ERROR_ALLOC, 0, 93, 0);
	}
}

// a font glyph as a byte a pixel, the cell's size
static uint8_t *glyph_bits(const struct xs_font *font, uint16_t c, int width, int height) {
	const uint8_t *rows = NULL;
	if (!xs_font_glyph(font, (uint8_t)(c >> 8), (uint8_t)c, &rows)) {
		return NULL;
	}
	uint8_t *bits = calloc((size_t)width * (size_t)height, 1);
	for (int y = 0; bits && rows && y < height; y++) {
		for (int x = 0; x < width && x < 8; x++) {
			bits[(size_t)y * (size_t)width + (size_t)x] = (rows[y] & (0x80 >> x)) != 0;
		}
	}
	return bits;
}

// CreateGlyphCursor: id, source font, mask font, source char, mask char,
// fore rgb, back rgb
static void create_glyph_cursor(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	uint32_t source_id = xin32(in, 8), mask_id = xin32(in, 12);
	const struct xs_font *source = xs_font_find(xserver, source_id);
	const struct xs_font *mask = mask_id ? xs_font_find(xserver, mask_id) : NULL;
	if (!source || (mask_id && !mask)) {
		xs_error(client, XS_ERROR_FONT, source ? mask_id : source_id, 94, 0);
		return;
	}
	uint16_t source_char = xin16(in, 16), mask_char = xin16(in, 18);
	const uint8_t *rows;
	if (!xs_font_glyph(source, (uint8_t)(source_char >> 8), (uint8_t)source_char, &rows)) {
		xs_error(client, XS_ERROR_VALUE, source_char, 94, 0);
		return;
	}
	struct xs_cursor *cursor = cursor_new(client, xin32(in, 4), 94, 0);
	if (!cursor) {
		return;
	}
	colors(in, 20, cursor->fore, cursor->back);
	if (xs_font_is_cursor(source)) {
		// XCreateFontCursor: the theme's take on the shape
		cursor->name = source_char / 2 < FONT_SHAPES ? font_shapes[source_char / 2] : "default";
		return;
	}
	// glyphs of a text font, hotspot at the origin: the left of the
	// baseline
	int w = xs_font_width(source), h = xs_font_ascent(source) + xs_font_descent(source);
	uint8_t *source_bits = glyph_bits(source, source_char, w, h);
	uint8_t *mask_bits = mask ? glyph_bits(mask, mask_char, w, h) : NULL;
	if (!mask && source_bits) {
		mask_bits = malloc((size_t)w * (size_t)h);
		if (mask_bits) {
			memcpy(mask_bits, source_bits, (size_t)w * (size_t)h);
		}
	}
	if (!source_bits || !mask_bits || !bits_cursor(cursor, source_bits, mask_bits, w, h, 0,
			xs_font_ascent(source) - 1)) {
		if (!cursor->source) {
			free(source_bits);
			free(mask_bits);
		}
		cursor_free_id(cursor);
		xs_error(client, XS_ERROR_ALLOC, 0, 94, 0);
	}
}

// an ARGB cursor's pixels are premultiplied, RENDER says so. some send
// theirs straight (unity's: clear pixels still white, a white square where
// its cursor should be). premultiplied, no color is brighter than its
// alpha: one that is says they're straight, and they're made premultiplied
static void premultiply_if_straight(uint32_t *pixels, size_t n) {
	bool straight = false;
	for (size_t i = 0; i < n && !straight; i++) {
		uint32_t a = pixels[i] >> 24;
		straight = (pixels[i] >> 16 & 0xff) > a || (pixels[i] >> 8 & 0xff) > a
			|| (pixels[i] & 0xff) > a;
	}
	for (size_t i = 0; straight && i < n; i++) {
		uint32_t p = pixels[i], a = p >> 24;
		uint32_t r = ((p >> 16 & 0xff) * a + 127) / 255, g = ((p >> 8 & 0xff) * a + 127) / 255,
			b = ((p & 0xff) * a + 127) / 255;
		pixels[i] = a << 24 | r << 16 | g << 8 | b;
	}
}

bool xs_cursor_create_argb(struct xs_client *client, uint32_t id, uint32_t *pixels, int width,
		int height, int hot_x, int hot_y, uint8_t major, uint8_t minor) {
	premultiply_if_straight(pixels, (size_t)width * (size_t)height);
	struct xs_cursor *cursor = cursor_new(client, id, major, minor);
	if (!cursor) {
		free(pixels);
		return false;
	}
	cursor->images = calloc(1, sizeof(*cursor->images));
	if (!cursor->images) {
		free(pixels);
		cursor_free_id(cursor);
		xs_error(client, XS_ERROR_ALLOC, id, major, minor);
		return false;
	}
	cursor->len = 1;
	cursor->images[0] = (struct xs_cursor_image){
		.pixels = pixels,
		.width = (uint16_t)width,
		.height = (uint16_t)height,
		.hot_x = (uint16_t)hot_x,
		.hot_y = (uint16_t)hot_y,
	};
	return true;
}

// RENDER's CreateAnimCursor: id, then a cursor and a delay (ms) a frame.
// each frame's look copied, they're made once and never change
void xs_cursor_create_animated(struct xs_client *client, const struct xin *in, uint8_t major,
		uint8_t minor) {
	struct flame_xserver *xserver = client->xserver;
	size_t frames = in->len > 8 ? (in->len - 8) / 8 : 0;
	if (!frames) {
		xs_error(client, XS_ERROR_LENGTH, 0, major, minor);
		return;
	}
	for (size_t i = 0; i < frames; i++) {
		struct xs_cursor *frame = get_cursor(client, xin32(in, 8 + i * 8), major, minor);
		if (!frame) {
			return;
		}
		if (!frame->len) {
			// a theme cursor can't be a frame of pixels
			xs_error(client, XS_ERROR_MATCH, frame->id, major, minor);
			return;
		}
	}
	struct xs_cursor *cursor = cursor_new(client, xin32(in, 4), major, minor);
	if (!cursor) {
		return;
	}
	cursor->images = calloc(frames, sizeof(*cursor->images));
	for (size_t i = 0; cursor->images && i < frames; i++) {
		struct xs_cursor_image *from = &xs_cursor_find(xserver, xin32(in, 8 + i * 8))->images[0];
		size_t bytes = (size_t)from->width * from->height * 4;
		struct xs_cursor_image *to = &cursor->images[i];
		*to = *from;
		to->delay = xin32(in, 12 + i * 8);
		to->pixels = malloc(bytes);
		if (!to->pixels) {
			break;
		}
		memcpy(to->pixels, from->pixels, bytes);
		cursor->len++;
	}
	if (cursor->len != frames) {
		cursor_free_id(cursor);
		xs_error(client, XS_ERROR_ALLOC, 0, major, minor);
	}
}

// RecolorCursor: id, fore rgb, back rgb. only bitmaps have colors to change,
// theme and ARGB ones look how they look
static void recolor_cursor(struct xs_client *client, const struct xin *in) {
	struct xs_cursor *cursor = get_cursor(client, xin32(in, 4), 96, 0);
	if (!cursor || !cursor->source) {
		return;
	}
	colors(in, 8, cursor->fore, cursor->back);
	paint_bits(cursor);
	cursor->serial = ++serials;
	xs_cursor_changed(client->xserver);
}

void xs_cursor_request(struct xs_client *client, const struct xin *in) {
	switch (xin8(in, 0)) {
	case 93:
		create_cursor(client, in);
		break;
	case 94:
		create_glyph_cursor(client, in);
		break;
	case 95: { // FreeCursor
		struct xs_cursor *cursor = get_cursor(client, xin32(in, 4), 95, 0);
		if (cursor) {
			cursor_free_id(cursor);
		}
		break;
	}
	case 96:
		recolor_cursor(client, in);
		break;
	}
}

void xs_cursor_client_gone(struct flame_xserver *xserver, struct xs_client *client) {
	struct xs_cursor *cursor, *tmp;
	if (!client) {
		// the server's going: windows and grabs went first, nothing's left
		// to show them
		wl_list_for_each_safe(cursor, tmp, &xserver->cursors, link) {
			cursor->refs = 1;
			xs_cursor_unref(cursor);
		}
		return;
	}
	wl_list_for_each_safe(cursor, tmp, &xserver->cursors, link) {
		if (cursor->owner == client && cursor->id) {
			cursor_free_id(cursor);
		}
	}
	// windows it showed them on are gone with it, or someone else's
	wl_list_for_each(cursor, &xserver->cursors, link) {
		if (cursor->owner == client) {
			cursor->owner = NULL;
		}
	}
	if (client->cursor_hides) {
		client->cursor_hides = 0;
		xs_cursor_changed(xserver);
	}
}

bool xs_cursor_id_in_use(struct flame_xserver *xserver, uint32_t id) {
	return xs_cursor_find(xserver, id) != NULL;
}

size_t xs_cursor_ids(struct flame_xserver *xserver, uint32_t base, uint32_t *out, size_t max) {
	size_t len = 0;
	struct xs_cursor *cursor;
	wl_list_for_each(cursor, &xserver->cursors, link) {
		if (cursor->id && (cursor->id & ~0x1fffffu) == base && (!out || len < max)) {
			if (out) {
				out[len] = cursor->id;
			}
			len++;
		}
	}
	return len;
}

bool xserver_cursor_hidden(const struct flame_xserver *xserver) {
	const struct xs_client *client;
	wl_list_for_each(client, &xserver->clients, link) {
		if (client->cursor_hides > 0) {
			return true;
		}
	}
	return false;
}
