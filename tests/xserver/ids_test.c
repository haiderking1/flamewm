// resource ids: one namespace per client, nobody takes another's, and
// XC-MISC hands out free ones. run by the harness: make test
#include <stdio.h>
#include <stdlib.h>
#include <xcb/xc_misc.h>
#include <xcb/xcb.h>

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

static uint8_t error_of(xcb_connection_t *c, xcb_void_cookie_t cookie) {
	xcb_generic_error_t *e = xcb_request_check(c, cookie);
	uint8_t code = e ? e->error_code : 0;
	free(e);
	return code;
}

int main(void) {
	xcb_connection_t *a = xcb_connect(NULL, NULL), *b = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(a) || xcb_connection_has_error(b)) {
		return 2;
	}
	xcb_screen_t *s = xcb_setup_roots_iterator(xcb_get_setup(a)).data;
	xcb_window_t w = xcb_generate_id(a);
	CHECK(!error_of(a, xcb_create_window_checked(a, 24, w, s->root, 0, 0, 10, 10, 0,
		XCB_WINDOW_CLASS_INPUT_OUTPUT, s->root_visual, 0, NULL)));
	// a pixmap or a gc can't have the window's id
	CHECK(error_of(a, xcb_create_pixmap_checked(a, 24, w, w, 4, 4)) == XCB_ID_CHOICE);
	CHECK(error_of(a, xcb_create_gc_checked(a, w, w, 0, NULL)) == XCB_ID_CHOICE);
	// another client can't make one in a's range
	xcb_gcontext_t theirs = xcb_generate_id(a);
	CHECK(error_of(b, xcb_create_gc_checked(b, theirs, s->root, 0, NULL)) == XCB_ID_CHOICE);
	CHECK(!error_of(a, xcb_create_gc_checked(a, theirs, w, 0, NULL)));

	// XC-MISC: free ids, none of them taken
	xcb_xc_misc_get_xid_range_reply_t *range = xcb_xc_misc_get_xid_range_reply(a,
		xcb_xc_misc_get_xid_range(a), NULL);
	CHECK(range && range->count > 1000);
	if (range) {
		CHECK(range->start_id != w && range->start_id != theirs);
		CHECK(!(w >= range->start_id && w < range->start_id + range->count));
	}
	free(range);
	xcb_xc_misc_get_xid_list_reply_t *list = xcb_xc_misc_get_xid_list_reply(a,
		xcb_xc_misc_get_xid_list(a, 8), NULL);
	CHECK(list && list->ids_len == 8);
	if (list) {
		uint32_t *ids = xcb_xc_misc_get_xid_list_ids(list);
		for (int i = 0; i < 8; i++) {
			CHECK(ids[i] != w && ids[i] != theirs);
			CHECK(!error_of(a, xcb_create_pixmap_checked(a, 24, ids[i], w, 2, 2)));
		}
	}
	free(list);

	xcb_disconnect(b);
	xcb_disconnect(a);
	if (failures) {
		fprintf(stderr, "ids_test: %d failed\n", failures);
		return 1;
	}
	printf("ids: all good\n");
	return 0;
}
