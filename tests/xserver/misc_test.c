// XINERAMA, MIT-SCREEN-SAVER, DPMS, XTEST, XI 1, GLX pbuffers against
// flame's X server, run by the harness: make test
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <xcb/dpms.h>
#include <xcb/glx.h>
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

	// at least a byte after the names: chromium's parser reads one more
	// and aborts every CEF and electron app on X when it isn't there
	xcb_input_list_input_devices_reply_t *list = xcb_input_list_input_devices_reply(c,
		xcb_input_list_input_devices(c), NULL);
	CHECK(list && list->devices_len == 4);
	if (list) {
		xcb_str_iterator_t name = xcb_input_list_input_devices_names_iterator(list);
		const char *end = (const char *)name.data;
		for (; name.rem; xcb_str_next(&name)) {
			end = xcb_str_name(name.data) + xcb_str_name_length(name.data);
		}
		CHECK(end - (const char *)list < 32 + 4 * (long)list->length);
	}
	free(list);

	// device properties: none, no error (chromium's touch setup asks)
	xcb_input_get_device_property_reply_t *prop = xcb_input_get_device_property_reply(c,
		xcb_input_get_device_property(c, XCB_ATOM_STRING, XCB_ATOM_ANY, 0, 1, 4, 0), NULL);
	CHECK(prop && prop->type == XCB_NONE && prop->device_id == 4);
	free(prop);
	xcb_input_list_device_properties_reply_t *props = xcb_input_list_device_properties_reply(c,
		xcb_input_list_device_properties(c, 4), NULL);
	CHECK(props && props->num_atoms == 0);
	free(props);
	error = NULL;
	prop = xcb_input_get_device_property_reply(c,
		xcb_input_get_device_property(c, XCB_ATOM_STRING, XCB_ATOM_ANY, 0, 1, 42, 0), &error);
	CHECK(!prop && error); // no such device
	free(prop);
	free(error);

	// the rest of XI1 answers like Xorg: wine's desktop process asks for the
	// button mapping and dies on an error
	xcb_input_get_device_button_mapping_reply_t *map =
		xcb_input_get_device_button_mapping_reply(c,
			xcb_input_get_device_button_mapping(c, 4), NULL);
	CHECK(map && map->map_size >= 3 && xcb_input_get_device_button_mapping_map(map)[0] == 1
		&& xcb_input_get_device_button_mapping_map(map)[2] == 3);
	free(map);
	error = NULL;
	map = xcb_input_get_device_button_mapping_reply(c,
		xcb_input_get_device_button_mapping(c, 5), &error);
	CHECK(!map && error && error->error_code == XCB_MATCH); // a keyboard has no buttons
	free(map);
	free(error);
	xcb_input_get_device_modifier_mapping_reply_t *mods =
		xcb_input_get_device_modifier_mapping_reply(c,
			xcb_input_get_device_modifier_mapping(c, 5), NULL);
	CHECK(mods && mods->keycodes_per_modifier >= 1);
	free(mods);
	xcb_input_get_device_key_mapping_reply_t *keys = xcb_input_get_device_key_mapping_reply(c,
		xcb_input_get_device_key_mapping(c, 5, 38, 1), NULL);
	CHECK(keys && keys->keysyms_per_keycode >= 1 && xcb_input_get_device_key_mapping_keysyms(
		keys)[0] == 'a');
	free(keys);
	xcb_input_get_device_focus_reply_t *focus = xcb_input_get_device_focus_reply(c,
		xcb_input_get_device_focus(c, 5), NULL);
	CHECK(focus != NULL);
	free(focus);
	xcb_input_query_device_state_reply_t *state = xcb_input_query_device_state_reply(c,
		xcb_input_query_device_state(c, 4), NULL);
	CHECK(state && state->num_classes == 2);
	free(state);
	xcb_input_get_feedback_control_reply_t *feedback = xcb_input_get_feedback_control_reply(c,
		xcb_input_get_feedback_control(c, 4), NULL);
	CHECK(feedback && feedback->num_feedbacks == 0);
	free(feedback);
	xcb_input_grab_device_reply_t *grab = xcb_input_grab_device_reply(c,
		xcb_input_grab_device(c, xcb_setup_roots_iterator(xcb_get_setup(c)).data->root,
			XCB_CURRENT_TIME, 0, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, 0, 4, NULL), NULL);
	CHECK(grab && grab->status == XCB_GRAB_STATUS_SUCCESS);
	free(grab);
	CHECK(!xcb_request_check(c, xcb_input_ungrab_device_checked(c, XCB_CURRENT_TIME, 4)));
	xcb_input_get_device_motion_events_reply_t *motion =
		xcb_input_get_device_motion_events_reply(c,
			xcb_input_get_device_motion_events(c, 0, XCB_CURRENT_TIME, 4), NULL);
	CHECK(motion && motion->num_events == 0);
	free(motion);
	// and the core pointer has as many buttons as XI says
	xcb_get_pointer_mapping_reply_t *core = xcb_get_pointer_mapping_reply(c,
		xcb_get_pointer_mapping(c), NULL);
	CHECK(core && core->map_len == 9);
	free(core);
}

// a configure that changes nothing sends nothing, like Xorg: toolkits take
// every ConfigureNotify for a move (steam re-placed its menus by them)
static int configure_notifies(xcb_connection_t *c) {
	free(xcb_get_input_focus_reply(c, xcb_get_input_focus(c), NULL));
	int n = 0;
	xcb_generic_event_t *e;
	while ((e = xcb_poll_for_event(c))) {
		n += (e->response_type & 0x7f) == XCB_CONFIGURE_NOTIFY;
		free(e);
	}
	return n;
}

static void test_configure_noop(xcb_connection_t *c, xcb_screen_t *s) {
	xcb_window_t w = xcb_generate_id(c);
	uint32_t values[] = {1, XCB_EVENT_MASK_STRUCTURE_NOTIFY};
	xcb_create_window(c, XCB_COPY_FROM_PARENT, w, s->root, 10, 20, 100, 50, 0,
		XCB_WINDOW_CLASS_INPUT_OUTPUT, s->root_visual,
		XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, values);
	xcb_map_window(c, w);
	configure_notifies(c);
	uint32_t same[] = {10, 20, 100, 50, XCB_STACK_MODE_ABOVE};
	xcb_configure_window(c, w, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y
		| XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT | XCB_CONFIG_WINDOW_STACK_MODE,
		same);
	CHECK(configure_notifies(c) == 0); // where it was, on top already
	uint32_t moved[] = {11, 20};
	xcb_configure_window(c, w, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, moved);
	CHECK(configure_notifies(c) == 1);
	xcb_destroy_window(c, w);
}

// pbuffers, what ANGLE (chromium's GL) makes current first thing
static void test_glx_pbuffer(xcb_connection_t *c) {
	xcb_glx_get_fb_configs_reply_t *f = xcb_glx_get_fb_configs_reply(c,
		xcb_glx_get_fb_configs(c, 0), NULL);
	CHECK(f && f->num_FB_configs > 0);
	uint32_t config = 0;
	if (f) {
		uint32_t *pairs = xcb_glx_get_fb_configs_property_list(f);
		for (uint32_t i = 0; i < f->num_properties; i++) {
			if (pairs[2 * i] == 0x8013) { // GLX_FBCONFIG_ID
				config = pairs[2 * i + 1];
			}
		}
	}
	free(f);
	xcb_glx_pbuffer_t pbuffer = xcb_generate_id(c);
	uint32_t attribs[] = {0x8041, 64, 0x8040, 32}; // GLX_PBUFFER_WIDTH, _HEIGHT
	CHECK(!xcb_request_check(c, xcb_glx_create_pbuffer_checked(c, 0, config, pbuffer, 2,
		attribs)));
	xcb_glx_get_drawable_attributes_reply_t *a = xcb_glx_get_drawable_attributes_reply(c,
		xcb_glx_get_drawable_attributes(c, pbuffer), NULL);
	uint32_t width = 0, height = 0, type = 0;
	if (a) {
		uint32_t *pairs = xcb_glx_get_drawable_attributes_attribs(a);
		for (uint32_t i = 0; i < a->num_attribs; i++) {
			width = pairs[2 * i] == 0x801d ? pairs[2 * i + 1] : width;
			height = pairs[2 * i] == 0x801e ? pairs[2 * i + 1] : height;
			type = pairs[2 * i] == 0x8010 ? pairs[2 * i + 1] : type;
		}
	}
	CHECK(width == 64 && height == 32 && type == 4);
	free(a);
	CHECK(!xcb_request_check(c, xcb_glx_destroy_pbuffer_checked(c, pbuffer)));
	xcb_generic_error_t *error = NULL;
	a = xcb_glx_get_drawable_attributes_reply(c, xcb_glx_get_drawable_attributes(c, pbuffer),
		&error);
	CHECK(!a && error); // gone
	free(a);
	free(error);
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
	test_glx_pbuffer(c);
	test_configure_noop(c, s);
	test_flame_windows(c, s); // last: every window goes
	xcb_disconnect(c);
	if (failures) {
		fprintf(stderr, "misc_test: %d failed\n", failures);
		return 1;
	}
	printf("misc: all good\n");
	return 0;
}
