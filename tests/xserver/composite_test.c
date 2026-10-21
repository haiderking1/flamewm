// Composite against flame's X server: redirecting and its rules, window
// pixmaps that see the window's pixels, border clips, the overlay window.
// run by the harness: make test
#include <stdio.h>
#include <stdlib.h>
#include <xcb/composite.h>
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

static xcb_window_t window(xcb_connection_t *conn, xcb_window_t parent, int x, int y, int w,
		int h) {
	xcb_window_t id = xcb_generate_id(conn);
	xcb_create_window(conn, 24, id, parent, (int16_t)x, (int16_t)y, (uint16_t)w, (uint16_t)h, 0,
		XCB_WINDOW_CLASS_INPUT_OUTPUT, s->root_visual, 0, NULL);
	xcb_map_window(conn, id);
	return id;
}

// the error code a checked request got, 0 for none
static int error_of(xcb_void_cookie_t cookie) {
	xcb_generic_error_t *err = xcb_request_check(c, cookie);
	int code = err ? err->error_code : 0;
	free(err);
	return code;
}

static uint32_t pixel(xcb_drawable_t d, int x, int y) {
	xcb_get_image_reply_t *img = xcb_get_image_reply(c,
		xcb_get_image(c, XCB_IMAGE_FORMAT_Z_PIXMAP, d, (int16_t)x, (int16_t)y, 1, 1, ~0u),
		NULL);
	uint32_t v = img ? *(uint32_t *)xcb_get_image_data(img) & 0xffffff : 0xdeadbeef;
	free(img);
	return v;
}

static void fill(xcb_drawable_t d, uint32_t color, int x, int y, int w, int h) {
	xcb_gcontext_t gc = xcb_generate_id(c);
	xcb_create_gc(c, gc, d, XCB_GC_FOREGROUND, &color);
	xcb_poly_fill_rectangle(c, d, gc, 1,
		&(xcb_rectangle_t){(int16_t)x, (int16_t)y, (uint16_t)w, (uint16_t)h});
	xcb_free_gc(c, gc);
}

static void test_redirect(void) {
	xcb_window_t w = window(c, s->root, 0, 0, 50, 50);
	CHECK(!error_of(xcb_composite_redirect_window_checked(c, w,
		XCB_COMPOSITE_REDIRECT_AUTOMATIC)));
	// the same client again
	CHECK(error_of(xcb_composite_redirect_window_checked(c, w,
		XCB_COMPOSITE_REDIRECT_AUTOMATIC)) == XCB_ACCESS);
	CHECK(!error_of(xcb_composite_unredirect_window_checked(c, w,
		XCB_COMPOSITE_REDIRECT_AUTOMATIC)));
	CHECK(error_of(xcb_composite_unredirect_window_checked(c, w,
		XCB_COMPOSITE_REDIRECT_AUTOMATIC)) == XCB_VALUE);
	// the root can't be, its children can
	CHECK(error_of(xcb_composite_redirect_window_checked(c, s->root,
		XCB_COMPOSITE_REDIRECT_AUTOMATIC)) == XCB_MATCH);
	CHECK(!error_of(xcb_composite_redirect_subwindows_checked(c, s->root,
		XCB_COMPOSITE_REDIRECT_MANUAL)));
	// one compositing manager at a time
	xcb_connection_t *other = xcb_connect(NULL, NULL);
	xcb_generic_error_t *err = xcb_request_check(other,
		xcb_composite_redirect_subwindows_checked(other, s->root,
			XCB_COMPOSITE_REDIRECT_MANUAL));
	CHECK(err && err->error_code == XCB_ACCESS);
	free(err);
	xcb_disconnect(other);
	// a manual redirect doesn't take windows off the screen: still viewable
	xcb_get_window_attributes_reply_t *a = xcb_get_window_attributes_reply(c,
		xcb_get_window_attributes(c, w), NULL);
	CHECK(a && a->map_state == XCB_MAP_STATE_VIEWABLE);
	free(a);
	CHECK(!error_of(xcb_composite_unredirect_subwindows_checked(c, s->root,
		XCB_COMPOSITE_REDIRECT_MANUAL)));
	xcb_destroy_window(c, w);
}

static void test_window_pixmap(void) {
	xcb_window_t w = window(c, s->root, 10, 10, 40, 30);
	fill(w, 0xff0000, 0, 0, 40, 30);
	xcb_pixmap_t p = xcb_generate_id(c);
	// a top-level counts as redirected already
	CHECK(!error_of(xcb_composite_name_window_pixmap_checked(c, w, p)));
	xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(c, xcb_get_geometry(c, p), NULL);
	CHECK(g && g->width == 40 && g->height == 30);
	free(g);
	CHECK(pixel(p, 5, 5) == 0xff0000);
	// it sees what's drawn after
	fill(w, 0x00ff00, 0, 0, 10, 10);
	CHECK(pixel(p, 5, 5) == 0x00ff00);
	// a resize gives the window new pixels, the pixmap keeps the old ones
	xcb_configure_window(c, w, XCB_CONFIG_WINDOW_WIDTH, (uint32_t[]){80});
	CHECK(pixel(p, 5, 5) == 0x00ff00);
	xcb_free_pixmap(c, p);

	// a child isn't, until redirected
	xcb_window_t child = window(c, w, 5, 5, 10, 10);
	p = xcb_generate_id(c);
	CHECK(error_of(xcb_composite_name_window_pixmap_checked(c, child, p)) == XCB_MATCH);
	xcb_composite_redirect_window(c, child, XCB_COMPOSITE_REDIRECT_AUTOMATIC);
	fill(child, 0x0000ff, 0, 0, 10, 10);
	CHECK(!error_of(xcb_composite_name_window_pixmap_checked(c, child, p)));
	CHECK(pixel(p, 2, 2) == 0x0000ff);
	xcb_free_pixmap(c, p);

	// not showing: nothing to name
	xcb_unmap_window(c, w);
	p = xcb_generate_id(c);
	CHECK(error_of(xcb_composite_name_window_pixmap_checked(c, w, p)) == XCB_MATCH);
	xcb_destroy_window(c, w);
}

static void test_border_clip(void) {
	xcb_window_t w = window(c, s->root, 100, 100, 50, 50);
	xcb_window_t above = window(c, s->root, 130, 100, 50, 50);
	xcb_xfixes_region_t r = xcb_generate_id(c);
	xcb_composite_create_region_from_border_clip(c, r, w);
	xcb_xfixes_fetch_region_reply_t *f = xcb_xfixes_fetch_region_reply(c,
		xcb_xfixes_fetch_region(c, r), NULL);
	// the right 20 are under the other one
	CHECK(f && f->extents.x == 0 && f->extents.width == 30 && f->extents.height == 50);
	free(f);
	xcb_xfixes_destroy_region(c, r);
	xcb_destroy_window(c, above);
	xcb_destroy_window(c, w);
}

static void test_overlay(void) {
	xcb_composite_get_overlay_window_reply_t *o = xcb_composite_get_overlay_window_reply(c,
		xcb_composite_get_overlay_window(c, s->root), NULL);
	CHECK(o && o->overlay_win);
	xcb_composite_get_overlay_window_reply_t *again = xcb_composite_get_overlay_window_reply(c,
		xcb_composite_get_overlay_window(c, s->root), NULL);
	CHECK(again && o && again->overlay_win == o->overlay_win);
	if (o) {
		// flame's: destroying it is ignored, like the root's
		xcb_destroy_window(c, o->overlay_win);
		xcb_get_window_attributes_reply_t *a = xcb_get_window_attributes_reply(c,
			xcb_get_window_attributes(c, o->overlay_win), NULL);
		CHECK(a != NULL);
		free(a);
	}
	xcb_composite_release_overlay_window(c, s->root);
	xcb_composite_release_overlay_window(c, s->root);
	if (o) {
		// released by everyone: it's gone
		xcb_get_window_attributes_reply_t *a = xcb_get_window_attributes_reply(c,
			xcb_get_window_attributes(c, o->overlay_win), NULL);
		CHECK(a == NULL);
		free(a);
	}
	free(o);
	free(again);
}

int main(void) {
	c = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(c)) {
		return 2;
	}
	s = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
	xcb_composite_query_version_reply_t *v = xcb_composite_query_version_reply(c,
		xcb_composite_query_version(c, 0, 4), NULL);
	CHECK(v && v->major_version == 0 && v->minor_version == 4);
	free(v);
	free(xcb_xfixes_query_version_reply(c, xcb_xfixes_query_version(c, 5, 0), NULL));
	test_redirect();
	test_window_pixmap();
	test_border_clip();
	test_overlay();
	xcb_disconnect(c);
	if (failures) {
		fprintf(stderr, "composite_test: %d failed\n", failures);
		return 1;
	}
	printf("composite: all good\n");
	return 0;
}
