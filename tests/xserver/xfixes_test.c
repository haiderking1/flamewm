// XFIXES regions against flame's X server: the math, regions from windows,
// GCs and pictures, and the clips and shapes set from them really clipping
// and shaping. run by the harness: make test
#include <stdio.h>
#include <stdlib.h>
#include <xcb/render.h>
#include <xcb/shape.h>
#include <xcb/xcb.h>
#include <xcb/xfixes.h>

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

static xcb_connection_t *c;
static xcb_screen_t *s;

static xcb_xfixes_region_t region(int n, const xcb_rectangle_t *rects) {
	xcb_xfixes_region_t r = xcb_generate_id(c);
	xcb_xfixes_create_region(c, r, (uint32_t)n, rects);
	return r;
}

// the region's rectangles, how many there are
static int fetch(xcb_xfixes_region_t r, xcb_rectangle_t *extents, xcb_rectangle_t *out,
		int max) {
	xcb_xfixes_fetch_region_reply_t *reply = xcb_xfixes_fetch_region_reply(c,
		xcb_xfixes_fetch_region(c, r), NULL);
	if (!reply) {
		return -1;
	}
	if (extents) {
		*extents = reply->extents;
	}
	int n = xcb_xfixes_fetch_region_rectangles_length(reply);
	xcb_rectangle_t *rects = xcb_xfixes_fetch_region_rectangles(reply);
	for (int i = 0; i < n && i < max; i++) {
		out[i] = rects[i];
	}
	free(reply);
	return n;
}

// how many pixels in a rows x cols image are set (nonzero), 32 bpp
static int lit(xcb_drawable_t d, int w, int h) {
	xcb_get_image_reply_t *img = xcb_get_image_reply(c,
		xcb_get_image(c, XCB_IMAGE_FORMAT_Z_PIXMAP, d, 0, 0, (uint16_t)w, (uint16_t)h, ~0u),
		NULL);
	if (!img) {
		return -1;
	}
	const uint32_t *px = (const uint32_t *)xcb_get_image_data(img);
	int n = 0;
	for (int i = 0; i < w * h; i++) {
		n += (px[i] & 0xffffff) != 0;
	}
	free(img);
	return n;
}

static void test_math(void) {
	xcb_rectangle_t a_r = {0, 0, 10, 10}, b_r = {5, 5, 10, 10};
	xcb_xfixes_region_t a = region(1, &a_r), b = region(1, &b_r), d = region(0, NULL);
	xcb_rectangle_t e, rects[16];

	xcb_xfixes_union_region(c, a, b, d);
	fetch(d, &e, rects, 16);
	CHECK(e.x == 0 && e.y == 0 && e.width == 15 && e.height == 15);

	xcb_xfixes_intersect_region(c, a, b, d);
	int n = fetch(d, &e, rects, 16);
	CHECK(n == 1 && rects[0].x == 5 && rects[0].width == 5 && rects[0].height == 5);

	xcb_xfixes_subtract_region(c, a, b, d);
	n = fetch(d, &e, rects, 16);
	CHECK(n == 2 && e.width == 10 && e.height == 10);

	// inside 0,0 20x20, what a isn't
	xcb_xfixes_invert_region(c, a, (xcb_rectangle_t){0, 0, 20, 20}, d);
	fetch(d, &e, rects, 16);
	CHECK(e.width == 20 && e.height == 20);
	xcb_xfixes_intersect_region(c, d, a, d);
	CHECK(fetch(d, NULL, rects, 16) == 0);

	xcb_xfixes_translate_region(c, a, 3, 4);
	n = fetch(a, &e, rects, 16);
	CHECK(n == 1 && e.x == 3 && e.y == 4 && e.width == 10);

	xcb_xfixes_region_extents(c, b, d);
	n = fetch(d, &e, rects, 16);
	CHECK(n == 1 && e.x == 5 && e.width == 10);

	xcb_xfixes_expand_region(c, b, d, 1, 2, 3, 4);
	fetch(d, &e, rects, 16);
	CHECK(e.x == 4 && e.y == 2 && e.width == 13 && e.height == 17);

	xcb_rectangle_t set[2] = {{0, 0, 1, 1}, {10, 10, 1, 1}};
	xcb_xfixes_set_region(c, d, 2, set);
	CHECK(fetch(d, NULL, rects, 16) == 2);
	xcb_xfixes_copy_region(c, d, a);
	CHECK(fetch(a, NULL, rects, 16) == 2);

	xcb_xfixes_destroy_region(c, a);
	xcb_xfixes_destroy_region(c, b);
	xcb_xfixes_destroy_region(c, d);
	// a gone region is BadRegion
	xcb_generic_error_t *err = NULL;
	xcb_xfixes_fetch_region_reply_t *r = xcb_xfixes_fetch_region_reply(c,
		xcb_xfixes_fetch_region(c, a), &err);
	CHECK(!r && err);
	free(r);
	free(err);
}

// a region from a window's shape, and a region made into one
static void test_window_shape(void) {
	xcb_window_t w = xcb_generate_id(c);
	xcb_create_window(c, 24, w, s->root, 10, 10, 100, 50, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
		s->root_visual, 0, NULL);
	xcb_xfixes_region_t r = xcb_generate_id(c);
	xcb_xfixes_create_region_from_window(c, r, w, XCB_SHAPE_SK_BOUNDING);
	xcb_rectangle_t e, rects[4];
	CHECK(fetch(r, &e, rects, 4) == 1 && e.width == 100 && e.height == 50);

	xcb_rectangle_t half = {0, 0, 50, 50};
	xcb_xfixes_set_region(c, r, 1, &half);
	xcb_xfixes_set_window_shape_region(c, w, XCB_SHAPE_SK_INPUT, 5, 0, r);
	xcb_shape_get_rectangles_reply_t *g = xcb_shape_get_rectangles_reply(c,
		xcb_shape_get_rectangles(c, w, XCB_SHAPE_SK_INPUT), NULL);
	CHECK(g && g->rectangles_len == 1);
	if (g && g->rectangles_len == 1) {
		xcb_rectangle_t *rr = xcb_shape_get_rectangles_rectangles(g);
		CHECK(rr->x == 5 && rr->width == 50);
	}
	free(g);
	// None: back to the plain rectangle
	xcb_xfixes_set_window_shape_region(c, w, XCB_SHAPE_SK_INPUT, 0, 0, XCB_NONE);
	xcb_shape_query_extents_reply_t *q = xcb_shape_query_extents_reply(c,
		xcb_shape_query_extents(c, w), NULL);
	CHECK(q && !q->bounding_shaped);
	free(q);
	xcb_xfixes_destroy_region(c, r);
	xcb_destroy_window(c, w);
}

// a GC clipped by a region only draws inside it
static void test_gc_clip(void) {
	xcb_pixmap_t p = xcb_generate_id(c);
	xcb_create_pixmap(c, 24, p, s->root, 20, 20);
	xcb_gcontext_t gc = xcb_generate_id(c);
	uint32_t black = 0;
	xcb_create_gc(c, gc, p, XCB_GC_FOREGROUND, &black);
	xcb_poly_fill_rectangle(c, p, gc, 1, &(xcb_rectangle_t){0, 0, 20, 20});

	xcb_rectangle_t square = {0, 0, 5, 5};
	xcb_xfixes_region_t r = region(1, &square);
	xcb_xfixes_set_gc_clip_region(c, gc, r, 2, 3);
	uint32_t white = 0xffffff;
	xcb_change_gc(c, gc, XCB_GC_FOREGROUND, &white);
	xcb_poly_fill_rectangle(c, p, gc, 1, &(xcb_rectangle_t){0, 0, 20, 20});
	CHECK(lit(p, 20, 20) == 25);

	xcb_xfixes_region_t back = xcb_generate_id(c);
	xcb_xfixes_create_region_from_gc(c, back, gc);
	xcb_rectangle_t e, rects[4];
	CHECK(fetch(back, &e, rects, 4) == 1 && e.width == 5);

	// None: no clip
	xcb_xfixes_set_gc_clip_region(c, gc, XCB_NONE, 0, 0);
	xcb_poly_fill_rectangle(c, p, gc, 1, &(xcb_rectangle_t){0, 0, 20, 20});
	CHECK(lit(p, 20, 20) == 400);

	xcb_xfixes_destroy_region(c, r);
	xcb_xfixes_destroy_region(c, back);
	xcb_free_gc(c, gc);
	xcb_free_pixmap(c, p);
}

// RENDER fills through a picture clipped by a region
static void test_picture_clip(void) {
	xcb_render_query_pict_formats_reply_t *formats = xcb_render_query_pict_formats_reply(c,
		xcb_render_query_pict_formats(c), NULL);
	if (!formats) {
		CHECK(!"no RENDER formats");
		return;
	}
	xcb_render_pictformat_t rgb = 0;
	xcb_render_pictforminfo_iterator_t it =
		xcb_render_query_pict_formats_formats_iterator(formats);
	for (; it.rem; xcb_render_pictforminfo_next(&it)) {
		if (it.data->depth == 24 && it.data->type == XCB_RENDER_PICT_TYPE_DIRECT) {
			rgb = it.data->id;
		}
	}
	free(formats);
	xcb_pixmap_t p = xcb_generate_id(c);
	xcb_create_pixmap(c, 24, p, s->root, 20, 20);
	xcb_gcontext_t gc = xcb_generate_id(c);
	uint32_t black = 0;
	xcb_create_gc(c, gc, p, XCB_GC_FOREGROUND, &black);
	xcb_poly_fill_rectangle(c, p, gc, 1, &(xcb_rectangle_t){0, 0, 20, 20});
	xcb_render_picture_t pic = xcb_generate_id(c);
	xcb_render_create_picture(c, pic, p, rgb, 0, NULL);

	xcb_rectangle_t strip = {0, 0, 20, 2};
	xcb_xfixes_region_t r = region(1, &strip);
	xcb_xfixes_set_picture_clip_region(c, pic, r, 0, 4);
	xcb_render_color_t white = {0xffff, 0xffff, 0xffff, 0xffff};
	xcb_render_fill_rectangles(c, XCB_RENDER_PICT_OP_SRC, pic, white, 1,
		&(xcb_rectangle_t){0, 0, 20, 20});
	CHECK(lit(p, 20, 20) == 40);

	xcb_xfixes_region_t back = xcb_generate_id(c);
	xcb_xfixes_create_region_from_picture(c, back, pic);
	xcb_rectangle_t e, rects[4];
	CHECK(fetch(back, &e, rects, 4) == 1 && e.height == 2);

	xcb_xfixes_destroy_region(c, r);
	xcb_xfixes_destroy_region(c, back);
	xcb_render_free_picture(c, pic);
	xcb_free_gc(c, gc);
	xcb_free_pixmap(c, p);
}

// replies that used to be missing: a client waiting on them would hang or die
static void test_cursor_names(void) {
	xcb_xfixes_get_cursor_name_reply_t *name = xcb_xfixes_get_cursor_name_reply(c,
		xcb_xfixes_get_cursor_name(c, 0), NULL);
	CHECK(name != NULL);
	free(name);
	xcb_xfixes_get_cursor_image_and_name_reply_t *both =
		xcb_xfixes_get_cursor_image_and_name_reply(c,
			xcb_xfixes_get_cursor_image_and_name(c), NULL);
	CHECK(both && both->width == 1 && both->height == 1);
	free(both);
	xcb_xfixes_change_cursor_by_name(c, 0, 4, "left");
	xcb_xfixes_change_save_set(c, XCB_XFIXES_SAVE_SET_MODE_INSERT,
		XCB_XFIXES_SAVE_SET_TARGET_NEAREST, XCB_XFIXES_SAVE_SET_MAPPING_MAP, s->root);
	xcb_generic_error_t *err = xcb_request_check(c,
		xcb_xfixes_change_cursor_by_name_checked(c, 0, 4, "left"));
	CHECK(!err);
	free(err);
}

int main(void) {
	c = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(c)) {
		return 2;
	}
	s = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
	xcb_xfixes_query_version_reply_t *v = xcb_xfixes_query_version_reply(c,
		xcb_xfixes_query_version(c, 5, 0), NULL);
	CHECK(v && v->major_version == 5);
	free(v);
	test_math();
	test_window_shape();
	test_gc_clip();
	test_picture_clip();
	test_cursor_names();
	xcb_disconnect(c);
	if (failures) {
		fprintf(stderr, "xfixes_test: %d failed\n", failures);
		return 1;
	}
	printf("xfixes: all good\n");
	return 0;
}
