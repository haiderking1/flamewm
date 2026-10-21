// DAMAGE against flame's X server: every report level, subtract and its
// parts, pixmaps, children and the root, run by the harness: make test
#include <stdio.h>
#include <stdlib.h>
#include <xcb/damage.h>
#include <xcb/render.h>
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
static uint8_t first_event;

#define MAX_EVENTS 64
static xcb_damage_notify_event_t got[MAX_EVENTS];
static int n_got;

// everything that came in up to now, the damage notifies kept
static void drain(void) {
	free(xcb_get_input_focus_reply(c, xcb_get_input_focus(c), NULL));
	n_got = 0;
	xcb_generic_event_t *e;
	while ((e = xcb_poll_for_event(c))) {
		if ((e->response_type & 0x7f) == first_event + XCB_DAMAGE_NOTIFY
				&& n_got < MAX_EVENTS) {
			got[n_got++] = *(xcb_damage_notify_event_t *)e;
		}
		free(e);
	}
}

static xcb_window_t window(xcb_window_t parent, int x, int y, int w, int h) {
	xcb_window_t id = xcb_generate_id(c);
	xcb_create_window(c, 24, id, parent, (int16_t)x, (int16_t)y, (uint16_t)w, (uint16_t)h, 0,
		XCB_WINDOW_CLASS_INPUT_OUTPUT, s->root_visual, 0, NULL);
	xcb_map_window(c, id);
	return id;
}

static xcb_gcontext_t gc_for(xcb_drawable_t d) {
	xcb_gcontext_t gc = xcb_generate_id(c);
	uint32_t white = 0xffffff;
	xcb_create_gc(c, gc, d, XCB_GC_FOREGROUND, &white);
	return gc;
}

static void fill(xcb_drawable_t d, xcb_gcontext_t gc, int x, int y, int w, int h) {
	xcb_poly_fill_rectangle(c, d, gc, 1,
		&(xcb_rectangle_t){(int16_t)x, (int16_t)y, (uint16_t)w, (uint16_t)h});
}

static int area_is(int i, int x, int y, int w, int h) {
	return i < n_got && got[i].area.x == x && got[i].area.y == y && got[i].area.width == w
		&& got[i].area.height == h;
}

static void test_levels(void) {
	xcb_window_t w = window(s->root, 20, 30, 100, 80);
	xcb_gcontext_t gc = gc_for(w);
	drain();

	// non-empty: told once, with the window all damaged from the start
	xcb_damage_damage_t d = xcb_generate_id(c);
	xcb_damage_create(c, d, w, XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY);
	drain();
	CHECK(n_got == 1 && got[0].damage == d && got[0].drawable == w);
	CHECK(n_got == 1 && got[0].geometry.x == 20 && got[0].geometry.y == 30
		&& got[0].geometry.width == 100);
	fill(w, gc, 0, 0, 5, 5);
	drain();
	CHECK(n_got == 0); // still not empty, nothing new to say
	xcb_damage_subtract(c, d, XCB_NONE, XCB_NONE);
	fill(w, gc, 0, 0, 5, 5);
	drain();
	CHECK(n_got == 1);
	xcb_damage_destroy(c, d);

	// raw: every drawing, as it is
	d = xcb_generate_id(c);
	xcb_damage_create(c, d, w, XCB_DAMAGE_REPORT_LEVEL_RAW_RECTANGLES);
	drain();
	fill(w, gc, 1, 2, 3, 4);
	fill(w, gc, 1, 2, 3, 4);
	drain();
	CHECK(n_got == 2 && area_is(0, 1, 2, 3, 4) && area_is(1, 1, 2, 3, 4));
	xcb_damage_destroy(c, d);

	// delta: only what's newly damaged
	d = xcb_generate_id(c);
	xcb_damage_create(c, d, w, XCB_DAMAGE_REPORT_LEVEL_DELTA_RECTANGLES);
	drain();
	xcb_damage_subtract(c, d, XCB_NONE, XCB_NONE);
	fill(w, gc, 10, 10, 10, 10);
	drain();
	CHECK(n_got == 1 && area_is(0, 10, 10, 10, 10));
	fill(w, gc, 10, 10, 10, 10);
	drain();
	CHECK(n_got == 0);
	// cut to the window
	fill(w, gc, 90, 70, 50, 50);
	drain();
	CHECK(n_got == 1 && area_is(0, 90, 70, 10, 10));
	xcb_damage_destroy(c, d);

	// bounding box: when it grows
	d = xcb_generate_id(c);
	xcb_damage_create(c, d, w, XCB_DAMAGE_REPORT_LEVEL_BOUNDING_BOX);
	drain();
	xcb_damage_subtract(c, d, XCB_NONE, XCB_NONE);
	fill(w, gc, 10, 10, 10, 10);
	drain();
	CHECK(n_got == 1 && area_is(0, 10, 10, 10, 10));
	fill(w, gc, 12, 12, 2, 2);
	drain();
	CHECK(n_got == 0);
	fill(w, gc, 30, 10, 5, 5);
	drain();
	CHECK(n_got == 1 && area_is(0, 10, 10, 25, 10));

	// subtract a part: the part comes back in parts, the rest is told again
	xcb_xfixes_region_t repair = xcb_generate_id(c), parts = xcb_generate_id(c);
	xcb_xfixes_create_region(c, repair, 1, &(xcb_rectangle_t){0, 0, 20, 100});
	xcb_xfixes_create_region(c, parts, 0, NULL);
	xcb_damage_subtract(c, d, repair, parts);
	drain();
	CHECK(n_got == 1 && area_is(0, 30, 10, 5, 5));
	xcb_xfixes_fetch_region_reply_t *r = xcb_xfixes_fetch_region_reply(c,
		xcb_xfixes_fetch_region(c, parts), NULL);
	CHECK(r && r->extents.x == 10 && r->extents.width == 10);
	free(r);
	xcb_damage_destroy(c, d);
	xcb_xfixes_destroy_region(c, repair);
	xcb_xfixes_destroy_region(c, parts);
	xcb_free_gc(c, gc);
	xcb_destroy_window(c, w);
}

// a child's drawing is its parent's damage too, the root's at its place on
// screen
static void test_tree(void) {
	xcb_window_t top = window(s->root, 50, 60, 200, 100);
	xcb_window_t child = window(top, 10, 20, 50, 50);
	xcb_gcontext_t gc = gc_for(child);
	xcb_damage_damage_t on_top = xcb_generate_id(c), on_root = xcb_generate_id(c);
	xcb_damage_create(c, on_top, top, XCB_DAMAGE_REPORT_LEVEL_RAW_RECTANGLES);
	xcb_damage_create(c, on_root, s->root, XCB_DAMAGE_REPORT_LEVEL_RAW_RECTANGLES);
	drain();
	fill(child, gc, 1, 1, 4, 4);
	drain();
	int top_ok = 0, root_ok = 0;
	for (int i = 0; i < n_got; i++) {
		top_ok += got[i].damage == on_top && area_is(i, 11, 21, 4, 4);
		root_ok += got[i].damage == on_root && area_is(i, 61, 81, 4, 4);
	}
	CHECK(top_ok == 1);
	CHECK(root_ok == 1);

	// Add: as if it were drawn
	xcb_xfixes_region_t region = xcb_generate_id(c);
	xcb_xfixes_create_region(c, region, 1, &(xcb_rectangle_t){0, 0, 2, 2});
	xcb_damage_add(c, child, region);
	drain();
	top_ok = 0;
	for (int i = 0; i < n_got; i++) {
		top_ok += got[i].damage == on_top && area_is(i, 10, 20, 2, 2);
	}
	CHECK(top_ok == 1);

	// the screen changes where a top-level goes away
	xcb_unmap_window(c, top);
	drain();
	root_ok = 0;
	for (int i = 0; i < n_got; i++) {
		root_ok += got[i].damage == on_root && area_is(i, 50, 60, 200, 100);
	}
	CHECK(root_ok == 1);

	// damages go with their drawable
	xcb_destroy_window(c, top);
	xcb_generic_error_t *err = xcb_request_check(c, xcb_damage_destroy_checked(c, on_top));
	CHECK(err != NULL);
	free(err);
	xcb_damage_destroy(c, on_root);
	xcb_xfixes_destroy_region(c, region);
	xcb_free_gc(c, gc);
}

// pixmaps: core drawing and RENDER
static void test_pixmap(void) {
	xcb_pixmap_t p = xcb_generate_id(c);
	xcb_create_pixmap(c, 24, p, s->root, 40, 40);
	xcb_gcontext_t gc = gc_for(p);
	xcb_damage_damage_t d = xcb_generate_id(c);
	xcb_damage_create(c, d, p, XCB_DAMAGE_REPORT_LEVEL_RAW_RECTANGLES);
	drain();
	CHECK(n_got == 0); // pixmaps don't start out damaged
	fill(p, gc, 5, 6, 7, 8);
	drain();
	CHECK(n_got == 1 && area_is(0, 5, 6, 7, 8) && got[0].geometry.width == 40);

	xcb_render_query_pict_formats_reply_t *formats = xcb_render_query_pict_formats_reply(c,
		xcb_render_query_pict_formats(c), NULL);
	xcb_render_pictformat_t rgb = 0;
	for (xcb_render_pictforminfo_iterator_t it =
			xcb_render_query_pict_formats_formats_iterator(formats);
			formats && it.rem; xcb_render_pictforminfo_next(&it)) {
		if (it.data->depth == 24 && it.data->type == XCB_RENDER_PICT_TYPE_DIRECT) {
			rgb = it.data->id;
		}
	}
	free(formats);
	xcb_render_picture_t pic = xcb_generate_id(c);
	xcb_render_create_picture(c, pic, p, rgb, 0, NULL);
	xcb_render_fill_rectangles(c, XCB_RENDER_PICT_OP_SRC, pic,
		(xcb_render_color_t){0xffff, 0, 0, 0xffff}, 1, &(xcb_rectangle_t){2, 3, 4, 5});
	drain();
	CHECK(n_got == 1 && area_is(0, 2, 3, 4, 5));
	xcb_render_free_picture(c, pic);

	xcb_free_pixmap(c, p);
	xcb_generic_error_t *err = xcb_request_check(c, xcb_damage_destroy_checked(c, d));
	CHECK(err != NULL);
	free(err);
	xcb_free_gc(c, gc);
}

int main(void) {
	c = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(c)) {
		return 2;
	}
	s = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
	const xcb_query_extension_reply_t *ext = xcb_get_extension_data(c, &xcb_damage_id);
	if (!ext || !ext->present) {
		fprintf(stderr, "damage_test: no DAMAGE\n");
		return 1;
	}
	first_event = ext->first_event;
	xcb_damage_query_version_reply_t *v = xcb_damage_query_version_reply(c,
		xcb_damage_query_version(c, 1, 1), NULL);
	CHECK(v && v->major_version == 1 && v->minor_version == 1);
	free(v);
	free(xcb_xfixes_query_version_reply(c, xcb_xfixes_query_version(c, 5, 0), NULL));
	test_levels();
	test_tree();
	test_pixmap();
	xcb_disconnect(c);
	if (failures) {
		fprintf(stderr, "damage_test: %d failed\n", failures);
		return 1;
	}
	printf("damage: all good\n");
	return 0;
}
