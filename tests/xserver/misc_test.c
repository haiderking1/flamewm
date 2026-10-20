// XINERAMA, MIT-SCREEN-SAVER, DPMS, XTEST against flame's X server, run by
// the harness: make test
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <xcb/dpms.h>
#include <xcb/screensaver.h>
#include <xcb/shape.h>
#include <xcb/xcb.h>
#include <xcb/xinput.h>
#include <xcb/xinerama.h>
#include <xcb/xtest.h>

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

static int to_harness, from_harness;

static void harness(const char *fmt, ...) {
	char line[128];
	va_list args;
	va_start(args, fmt);
	int len = vsnprintf(line, sizeof(line) - 1, fmt, args);
	va_end(args);
	line[len++] = '\n';
	if (write(to_harness, line, (size_t)len) != len) {
		exit(3);
	}
	char c;
	do {
		if (read(from_harness, &c, 1) != 1) {
			exit(3);
		}
	} while (c != '\n');
}

static void test_xinerama(xcb_connection_t *c) {
	harness("monitors 2");
	xcb_xinerama_query_screens_reply_t *r = xcb_xinerama_query_screens_reply(c,
		xcb_xinerama_query_screens(c), NULL);
	CHECK(r && r->number == 2);
	if (r) {
		xcb_xinerama_screen_info_t *s = xcb_xinerama_query_screens_screen_info(r);
		CHECK(s[0].x_org == 0 && s[0].width == 1920); // the primary first
		CHECK(s[1].x_org == 1920 && s[1].height == 1080);
	}
	free(r);
	xcb_xinerama_is_active_reply_t *a = xcb_xinerama_is_active_reply(c,
		xcb_xinerama_is_active(c), NULL);
	CHECK(a && a->state == 1);
	free(a);
}

static void test_idle(xcb_connection_t *c, xcb_screen_t *s) {
	nanosleep(&(struct timespec){0, 300000000}, NULL);
	xcb_screensaver_query_info_reply_t *r = xcb_screensaver_query_info_reply(c,
		xcb_screensaver_query_info(c, s->root), NULL);
	CHECK(r && r->ms_since_user_input >= 250);
	free(r);
	xcb_dpms_info_reply_t *d = xcb_dpms_info_reply(c, xcb_dpms_info(c), NULL);
	CHECK(d && !d->state);
	free(d);
}

// made up keys land in the focused window like real ones
static void test_xtest(xcb_connection_t *c, xcb_screen_t *s) {
	xcb_window_t w = xcb_generate_id(c);
	uint32_t mask = XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_BUTTON_PRESS;
	xcb_create_window(c, 24, w, s->root, 0, 0, 100, 100, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
		s->root_visual, XCB_CW_EVENT_MASK, &mask);
	xcb_map_window(c, w);
	free(xcb_get_input_focus_reply(c, xcb_get_input_focus(c), NULL));
	harness("focus %x", w);
	xcb_test_fake_input(c, XCB_KEY_PRESS, 38, XCB_CURRENT_TIME, s->root, 0, 0, 0);
	xcb_test_fake_input(c, XCB_KEY_RELEASE, 38, XCB_CURRENT_TIME, s->root, 0, 0, 0);
	xcb_test_fake_input(c, XCB_MOTION_NOTIFY, 0, XCB_CURRENT_TIME, s->root, 50, 50, 0);
	xcb_test_fake_input(c, XCB_BUTTON_PRESS, 1, XCB_CURRENT_TIME, s->root, 0, 0, 0);
	xcb_test_fake_input(c, XCB_BUTTON_RELEASE, 1, XCB_CURRENT_TIME, s->root, 0, 0, 0);
	free(xcb_get_input_focus_reply(c, xcb_get_input_focus(c), NULL));
	int keys = 0, buttons = 0;
	xcb_generic_event_t *e;
	while ((e = xcb_poll_for_event(c))) {
		uint8_t type = e->response_type & 0x7f;
		keys += type == XCB_KEY_PRESS && ((xcb_key_press_event_t *)e)->detail == 38;
		buttons += type == XCB_BUTTON_PRESS && ((xcb_button_press_event_t *)e)->event == w;
		free(e);
	}
	CHECK(keys == 1);
	CHECK(buttons == 1);
	// xdotool's ctrl+c: Control_L down makes the c a ctrl+c
	xcb_test_fake_input(c, XCB_KEY_PRESS, 37, XCB_CURRENT_TIME, s->root, 0, 0, 0);
	xcb_test_fake_input(c, XCB_KEY_PRESS, 54, XCB_CURRENT_TIME, s->root, 0, 0, 0);
	xcb_test_fake_input(c, XCB_KEY_RELEASE, 54, XCB_CURRENT_TIME, s->root, 0, 0, 0);
	xcb_test_fake_input(c, XCB_KEY_RELEASE, 37, XCB_CURRENT_TIME, s->root, 0, 0, 0);
	xcb_test_fake_input(c, XCB_KEY_PRESS, 54, XCB_CURRENT_TIME, s->root, 0, 0, 0);
	xcb_test_fake_input(c, XCB_KEY_RELEASE, 54, XCB_CURRENT_TIME, s->root, 0, 0, 0);
	free(xcb_get_input_focus_reply(c, xcb_get_input_focus(c), NULL));
	int ctrl_c = 0, plain_c = 0;
	while ((e = xcb_poll_for_event(c))) {
		xcb_key_press_event_t *k = (xcb_key_press_event_t *)e;
		if ((e->response_type & 0x7f) == XCB_KEY_PRESS && k->detail == 54) {
			ctrl_c += (k->state & XCB_MOD_MASK_CONTROL) != 0;
			plain_c += !(k->state & XCB_MOD_MASK_CONTROL);
		}
		free(e);
	}
	CHECK(ctrl_c == 1);
	CHECK(plain_c == 1);
}

// XI1 clients (old games, wine) open the slave devices
static void test_xi1(xcb_connection_t *c) {
	xcb_input_open_device_reply_t *r = xcb_input_open_device_reply(c,
		xcb_input_open_device(c, 4), NULL);
	CHECK(r && r->num_classes == 3);
	if (r) {
		xcb_input_input_class_info_t *info = xcb_input_open_device_class_info(r);
		CHECK(info[0].class_id == XCB_INPUT_INPUT_CLASS_BUTTON);
	}
	free(r);
	xcb_generic_error_t *error = NULL;
	r = xcb_input_open_device_reply(c, xcb_input_open_device(c, 2), &error);
	CHECK(!r && error); // a master: XI2's business
	free(r);
	free(error);
	CHECK(!xcb_request_check(c, xcb_input_close_device_checked(c, 4)));
}

// a client can't take flame's own windows away: the selection window survives
// everything under the root going, and stays put
static void test_flame_windows(xcb_connection_t *c, xcb_screen_t *s) {
	xcb_window_t w = xcb_generate_id(c);
	xcb_create_window(c, 24, w, s->root, 0, 0, 10, 10, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
		s->root_visual, 0, NULL);
	xcb_generic_error_t *error = xcb_request_check(c,
		xcb_reparent_window_checked(c, 0x102, w, 0, 0));
	CHECK(error && error->error_code == XCB_ACCESS);
	free(error);
	xcb_destroy_subwindows(c, s->root);
	xcb_get_window_attributes_reply_t *a = xcb_get_window_attributes_reply(c,
		xcb_get_window_attributes(c, 0x102), NULL);
	CHECK(a != NULL);
	free(a);
	a = xcb_get_window_attributes_reply(c, xcb_get_window_attributes(c, w), NULL);
	CHECK(a == NULL);
	free(a);
}

// half a window takes input: the pointer over the other half finds what's
// below it
static void test_shape(xcb_connection_t *c, xcb_screen_t *s) {
	const xcb_query_extension_reply_t *ext = xcb_get_extension_data(c, &xcb_shape_id);
	CHECK(ext && ext->present);
	xcb_window_t w = xcb_generate_id(c);
	xcb_create_window(c, 24, w, s->root, 300, 300, 200, 100, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
		s->root_visual, 0, NULL);
	xcb_map_window(c, w);
	xcb_shape_select_input(c, w, 1);
	xcb_rectangle_t left = {0, 0, 100, 100};
	xcb_shape_rectangles(c, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, 0, w, 0, 0, 1, &left);
	free(xcb_get_input_focus_reply(c, xcb_get_input_focus(c), NULL));
	int notifies = 0;
	xcb_generic_event_t *e;
	while ((e = xcb_poll_for_event(c))) {
		notifies += ext && (e->response_type & 0x7f) == ext->first_event;
		free(e);
	}
	CHECK(notifies == 1);
	harness("motion 350 350");
	xcb_query_pointer_reply_t *p = xcb_query_pointer_reply(c, xcb_query_pointer(c, s->root),
		NULL);
	CHECK(p && p->child == w);
	free(p);
	harness("motion 450 350");
	p = xcb_query_pointer_reply(c, xcb_query_pointer(c, s->root), NULL);
	CHECK(p && p->child != w);
	free(p);
	xcb_shape_get_rectangles_reply_t *r = xcb_shape_get_rectangles_reply(c,
		xcb_shape_get_rectangles(c, w, XCB_SHAPE_SK_INPUT), NULL);
	CHECK(r && r->rectangles_len == 1);
	if (r) {
		xcb_rectangle_t *rect = xcb_shape_get_rectangles_rectangles(r);
		CHECK(rect->width == 100 && rect->height == 100);
	}
	free(r);
	xcb_shape_query_extents_reply_t *q = xcb_shape_query_extents_reply(c,
		xcb_shape_query_extents(c, w), NULL);
	CHECK(q && !q->bounding_shaped && q->bounding_shape_extents_width == 200);
	free(q);
	// back to a rectangle
	xcb_shape_mask(c, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, w, 0, 0, XCB_NONE);
	p = xcb_query_pointer_reply(c, xcb_query_pointer(c, s->root), NULL);
	CHECK(p && p->child == w);
	free(p);
}

int main(void) {
	const char *fds = getenv("HARNESS_FDS");
	if (!fds || sscanf(fds, "%d %d", &to_harness, &from_harness) != 2) {
		fprintf(stderr, "run me with the harness\n");
		return 2;
	}
	xcb_connection_t *c = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(c)) {
		return 2;
	}
	xcb_screen_t *s = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
	test_xinerama(c);
	test_idle(c, s);
	test_xtest(c, s);
	test_shape(c, s);
	test_xi1(c);
	test_flame_windows(c, s); // last: every window goes
	xcb_disconnect(c);
	if (failures) {
		fprintf(stderr, "misc_test: %d failed\n", failures);
		return 1;
	}
	printf("misc: all good\n");
	return 0;
}
