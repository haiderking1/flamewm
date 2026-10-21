// core fonts against flame's X server, run by the harness: make test. the
// built-ins Xorg has (fixed, cursor), what they say about themselves, and
// text drawn with them pixel for pixel
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

// fixed's H, misc-fixed 6x13: rows of 6 bits from the top bit down
static const uint8_t glyph_h[13] = {0x00, 0x00, 0x88, 0x88, 0x88, 0x88, 0xf8, 0x88, 0x88,
	0x88, 0x88, 0x00, 0x00};

static uint8_t open_error(xcb_connection_t *c, const char *name) {
	xcb_font_t font = xcb_generate_id(c);
	xcb_generic_error_t *e = xcb_request_check(c, xcb_open_font_checked(c, font,
		(uint16_t)strlen(name), name));
	uint8_t code = e ? e->error_code : 0;
	free(e);
	if (!code) {
		xcb_close_font(c, font);
	}
	return code;
}

static void test_names(xcb_connection_t *c) {
	CHECK(open_error(c, "fixed") == 0);
	CHECK(open_error(c, "FiXeD") == 0); // case doesn't matter
	CHECK(open_error(c, "6x13") == 0);
	CHECK(open_error(c, "cursor") == 0); // XCreateFontCursor's
	CHECK(open_error(c, "-*-fixed-medium-r-*--13-*-*-*-*-*-iso8859-1") == 0);
	CHECK(open_error(c, "nothing-like-it") == XCB_NAME);
	// a pattern that would backtrack forever done naively: still an answer
	CHECK(open_error(c, "*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*b") == XCB_NAME);

	xcb_list_fonts_reply_t *list = xcb_list_fonts_reply(c, xcb_list_fonts(c, 10, 1, "*"),
		NULL);
	bool fixed = false, cursor = false;
	if (list) {
		for (xcb_str_iterator_t it = xcb_list_fonts_names_iterator(list); it.rem;
				xcb_str_next(&it)) {
			int len = xcb_str_name_length(it.data);
			fixed |= len == 5 && !memcmp(xcb_str_name(it.data), "fixed", 5);
			cursor |= len == 6 && !memcmp(xcb_str_name(it.data), "cursor", 6);
		}
	}
	CHECK(fixed && cursor);
	free(list);
	const char *other = "-misc-fixed-medium-r-normal--*-120-*-*-c-*-iso10646-1";
	list = xcb_list_fonts_reply(c, xcb_list_fonts(c, 10, (uint16_t)strlen(other), other), NULL);
	CHECK(list && list->names_len == 0); // what an xlib font set asks, nothing like it
	free(list);

	// one reply for the font, then the empty one that ends it
	xcb_list_fonts_with_info_cookie_t cookie = xcb_list_fonts_with_info(c, 10, 5, "fixed");
	int replies = 0;
	xcb_list_fonts_with_info_reply_t *info;
	while ((info = xcb_list_fonts_with_info_reply(c, cookie, NULL))) {
		if (info->name_len == 0) {
			free(info);
			break;
		}
		replies++;
		CHECK(info->font_ascent == 11 && info->font_descent == 2);
		CHECK(info->max_bounds.character_width == 6);
		free(info);
	}
	CHECK(replies == 1);
}

static void test_query(xcb_connection_t *c, xcb_screen_t *s) {
	xcb_font_t font = xcb_generate_id(c);
	xcb_open_font(c, font, 5, "fixed");
	xcb_query_font_reply_t *q = xcb_query_font_reply(c, xcb_query_font(c, font), NULL);
	CHECK(q && q->font_ascent == 11 && q->font_descent == 2);
	CHECK(q && q->min_char_or_byte2 == 0 && q->max_char_or_byte2 == 255);
	CHECK(q && q->char_infos_len == 256 && q->max_bounds.character_width == 6);
	if (q) {
		xcb_charinfo_t *chars = xcb_query_font_char_infos(q);
		CHECK(chars['H'].character_width == 6 && chars['H'].ascent == 11);
		CHECK(chars[0x80].character_width == 0); // not in iso8859-1
		bool named = false;
		xcb_intern_atom_reply_t *atom = xcb_intern_atom_reply(c,
			xcb_intern_atom(c, 0, 4, "FONT"), NULL);
		xcb_atom_t font_atom = atom ? atom->atom : XCB_NONE;
		free(atom);
		xcb_fontprop_t *props = xcb_query_font_properties(q);
		for (int i = 0; i < xcb_query_font_properties_length(q); i++) {
			named |= props[i].name == font_atom;
		}
		CHECK(named);
	}
	free(q);

	// a GC's font when it was never given one: fixed
	xcb_gcontext_t gc = xcb_generate_id(c);
	xcb_create_gc(c, gc, s->root, 0, NULL);
	q = xcb_query_font_reply(c, xcb_query_font(c, gc), NULL);
	CHECK(q && q->font_ascent == 11);
	free(q);

	xcb_char2b_t abc[3] = {{0, 'a'}, {0, 'b'}, {0, 'c'}};
	xcb_query_text_extents_reply_t *e = xcb_query_text_extents_reply(c,
		xcb_query_text_extents(c, font, 3, abc), NULL);
	CHECK(e && e->overall_width == 18 && e->overall_ascent == 11);
	free(e);

	xcb_close_font(c, font);
	xcb_generic_error_t *error = NULL;
	q = xcb_query_font_reply(c, xcb_query_font(c, font), &error);
	CHECK(!q && error && error->error_code == XCB_FONT);
	free(q);
	free(error);
	// a GC can't take a font that isn't there
	uint32_t bad = font;
	error = xcb_request_check(c, xcb_change_gc_checked(c, gc, XCB_GC_FONT, &bad));
	CHECK(error && error->error_code == XCB_FONT);
	free(error);
	xcb_free_gc(c, gc);
}

static uint32_t *pixels(xcb_connection_t *c, xcb_pixmap_t pixmap, int w, int h,
		xcb_get_image_reply_t **reply) {
	*reply = xcb_get_image_reply(c, xcb_get_image(c, XCB_IMAGE_FORMAT_Z_PIXMAP, pixmap, 0, 0,
		(uint16_t)w, (uint16_t)h, ~0u), NULL);
	return *reply ? (uint32_t *)xcb_get_image_data(*reply) : NULL;
}

static void test_draw(xcb_connection_t *c, xcb_screen_t *s) {
	enum { W = 24, H = 16 };
	xcb_pixmap_t pixmap = xcb_generate_id(c);
	xcb_create_pixmap(c, 24, pixmap, s->root, W, H);
	xcb_gcontext_t gc = xcb_generate_id(c);
	uint32_t black = 0;
	xcb_create_gc(c, gc, pixmap, XCB_GC_FOREGROUND, &black);
	xcb_rectangle_t all = {0, 0, W, H};
	xcb_poly_fill_rectangle(c, pixmap, gc, 1, &all);

	// PolyText8, the GC's default font: H's pixels and nothing else, the
	// baseline at y 12
	uint32_t white = 0xffffff;
	xcb_change_gc(c, gc, XCB_GC_FOREGROUND, &white);
	uint8_t item[] = {1, 0, 'H', 0};
	xcb_poly_text_8(c, pixmap, gc, 2, 12, sizeof(item), item);
	xcb_get_image_reply_t *r;
	uint32_t *p = pixels(c, pixmap, W, H, &r);
	CHECK(p);
	int wrong = 0;
	for (int y = 0; p && y < H; y++) {
		for (int x = 0; x < W; x++) {
			int gx = x - 2, gy = y - 1; // the cell's top: baseline 12 minus ascent 11
			bool on = gx >= 0 && gx < 6 && gy >= 0 && gy < 13 && glyph_h[gy] & (0x80 >> gx);
			wrong += (p[y * W + x] & 0xffffff) != (on ? 0xffffffu : 0);
		}
	}
	CHECK(wrong == 0);
	free(r);

	// ImageText16: its box in the background, a cell for each char
	uint32_t colors[2] = {0x0000ff, 0x00ff00};
	xcb_change_gc(c, gc, XCB_GC_FOREGROUND | XCB_GC_BACKGROUND, colors);
	xcb_char2b_t spaces[2] = {{0, ' '}, {0, ' '}};
	xcb_image_text_16(c, 2, pixmap, gc, 4, 13, spaces);
	p = pixels(c, pixmap, W, H, &r);
	CHECK(p && (p[2 * W + 4] & 0xffffff) == 0x00ff00); // inside: 4..16, 2..15
	CHECK(p && (p[14 * W + 15] & 0xffffff) == 0x00ff00);
	CHECK(p && (p[2 * W + 16] & 0xffffff) != 0x00ff00); // past two cells
	CHECK(p && (p[1 * W + 4] & 0xffffff) != 0x00ff00); // above the ascent
	free(r);

	// a font shift to a font that isn't there
	uint8_t shift[] = {255, 0, 0, 0, 1, 0, 0, 0};
	xcb_generic_error_t *e = xcb_request_check(c, xcb_poly_text_8_checked(c, pixmap, gc, 0,
		12, sizeof(shift), shift));
	CHECK(e && e->error_code == XCB_FONT);
	free(e);
	xcb_free_gc(c, gc);
	xcb_free_pixmap(c, pixmap);
}

int main(void) {
	xcb_connection_t *c = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(c)) {
		return 2;
	}
	xcb_screen_t *s = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
	test_names(c);
	test_query(c, s);
	test_draw(c, s);
	xcb_disconnect(c);
	if (failures) {
		fprintf(stderr, "font_test: %d failed\n", failures);
		return 1;
	}
	printf("fonts: all good\n");
	return 0;
}
