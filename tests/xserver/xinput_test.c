// XInput 2 and RandR against flame's X server, run by the harness: make test
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/randr.h>
#include <xcb/xcb.h>
#include <xcb/xinput.h>

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

static int to_harness, from_harness;

// a harness command, back once it's done
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

static uint8_t xi_opcode, randr_event;

// every event that arrives within a round trip, the XI2 ones by type
struct events {
	xcb_generic_event_t *list[64];
	int len;
};

static void collect(xcb_connection_t *c, struct events *out) {
	free(xcb_get_input_focus_reply(c, xcb_get_input_focus(c), NULL));
	out->len = 0;
	xcb_generic_event_t *e;
	while ((e = xcb_poll_for_event(c)) && out->len < 64) {
		out->list[out->len++] = e;
	}
}

static void release(struct events *events) {
	for (int i = 0; i < events->len; i++) {
		free(events->list[i]);
	}
	events->len = 0;
}

static xcb_ge_generic_event_t *xi_event(struct events *events, uint16_t type, int nth) {
	for (int i = 0; i < events->len; i++) {
		xcb_ge_generic_event_t *ge = (xcb_ge_generic_event_t *)events->list[i];
		if ((ge->response_type & 0x7f) == XCB_GE_GENERIC && ge->extension == xi_opcode
				&& ge->event_type == type && nth-- == 0) {
			return ge;
		}
	}
	return NULL;
}

static int core_count(struct events *events, uint8_t type) {
	int n = 0;
	for (int i = 0; i < events->len; i++) {
		n += (events->list[i]->response_type & 0x7f) == type;
	}
	return n;
}

static double fp3232(xcb_input_fp3232_t v) {
	return v.integral + v.frac / 4294967296.0;
}

static xcb_window_t make_window(xcb_connection_t *c, xcb_screen_t *s, uint32_t core_mask) {
	xcb_window_t w = xcb_generate_id(c);
	uint32_t values[] = {core_mask};
	xcb_create_window(c, 24, w, s->root, 0, 0, 200, 200, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
		s->root_visual, XCB_CW_EVENT_MASK, values);
	xcb_map_window(c, w);
	free(xcb_get_input_focus_reply(c, xcb_get_input_focus(c), NULL)); // others can use it
	return w;
}

static void select_xi(xcb_connection_t *c, xcb_window_t w, uint16_t device, uint32_t mask) {
	struct {
		xcb_input_event_mask_t head;
		uint32_t mask;
	} m = {{device, 1}, mask};
	xcb_input_xi_select_events(c, w, 1, &m.head);
}

static void test_devices(xcb_connection_t *c) {
	xcb_input_xi_query_version_reply_t *v = xcb_input_xi_query_version_reply(c,
		xcb_input_xi_query_version(c, 2, 4), NULL);
	CHECK(v && v->major_version == 2 && v->minor_version == 2);
	free(v);
	xcb_input_xi_query_device_reply_t *d = xcb_input_xi_query_device_reply(c,
		xcb_input_xi_query_device(c, XCB_INPUT_DEVICE_ALL), NULL);
	CHECK(d && d->num_infos == 4);
	if (!d) {
		return;
	}
	int scroll_classes = 0, pointers = 0;
	xcb_input_xi_device_info_iterator_t it = xcb_input_xi_query_device_infos_iterator(d);
	for (; it.rem; xcb_input_xi_device_info_next(&it)) {
		pointers += it.data->type == XCB_INPUT_DEVICE_TYPE_MASTER_POINTER;
		xcb_input_device_class_iterator_t ci = xcb_input_xi_device_info_classes_iterator(it.data);
		for (; ci.rem; xcb_input_device_class_next(&ci)) {
			if (ci.data->type == XCB_INPUT_DEVICE_CLASS_TYPE_SCROLL) {
				xcb_input_scroll_class_t *sc = (xcb_input_scroll_class_t *)ci.data;
				scroll_classes++;
				CHECK(fp3232(sc->increment) == 15);
			}
		}
	}
	CHECK(pointers == 1);
	CHECK(scroll_classes == 4); // master and slave, vertical and horizontal
	free(d);
}

// an XI2 client and a core client on the same window: each gets its own kind
static void test_pointer(xcb_connection_t *c, xcb_connection_t *core, xcb_screen_t *s) {
	xcb_window_t w = make_window(c, s, XCB_EVENT_MASK_BUTTON_PRESS);
	uint32_t core_mask = XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_POINTER_MOTION;
	xcb_change_window_attributes(core, w, XCB_CW_EVENT_MASK, &core_mask);
	free(xcb_get_input_focus_reply(core, xcb_get_input_focus(core), NULL));
	select_xi(c, w, XCB_INPUT_DEVICE_ALL_MASTER, XCB_INPUT_XI_EVENT_MASK_MOTION
		| XCB_INPUT_XI_EVENT_MASK_BUTTON_PRESS | XCB_INPUT_XI_EVENT_MASK_BUTTON_RELEASE
		| XCB_INPUT_XI_EVENT_MASK_ENTER | XCB_INPUT_XI_EVENT_MASK_LEAVE);
	struct events events, core_events;
	collect(c, &events);
	release(&events);

	harness("motion 10 20");
	collect(c, &events);
	xcb_input_enter_event_t *enter = (void *)xi_event(&events, XCB_INPUT_ENTER, 0);
	CHECK(enter && enter->event == w && enter->event_x >> 16 == 10);
	xcb_input_motion_event_t *motion = (void *)xi_event(&events, XCB_INPUT_MOTION, 0);
	CHECK(motion && motion->deviceid == 2 && motion->sourceid == 4);
	CHECK(motion && motion->event_y >> 16 == 20);
	release(&events);

	harness("button 1 1");
	collect(c, &events);
	collect(core, &core_events);
	xcb_input_button_press_event_t *press = (void *)xi_event(&events, XCB_INPUT_BUTTON_PRESS, 0);
	CHECK(press && press->detail == 1 && press->event == w);
	CHECK(core_count(&events, XCB_BUTTON_PRESS) == 0); // it selected XI2 instead
	CHECK(core_count(&core_events, XCB_BUTTON_PRESS) == 1);
	release(&events);
	release(&core_events);
	harness("button 1 0");
	collect(c, &events);
	CHECK(xi_event(&events, XCB_INPUT_BUTTON_RELEASE, 0) != NULL);
	release(&events);

	// smooth: the scroll valuator moves by the delta, the wheel button comes
	// after, marked as emulated. the first scroll sends the valuators as they
	// are first so toolkits have something to count from
	harness("scroll v 7.5");
	collect(c, &events);
	collect(core, &core_events);
	xcb_input_motion_event_t *prime = (void *)xi_event(&events, XCB_INPUT_MOTION, 0);
	xcb_input_motion_event_t *moved = (void *)xi_event(&events, XCB_INPUT_MOTION, 1);
	CHECK(prime && moved);
	if (prime && moved) {
		xcb_input_fp3232_t *a = xcb_input_button_press_axisvalues(prime);
		xcb_input_fp3232_t *b = xcb_input_button_press_axisvalues(moved);
		CHECK(xcb_input_button_press_axisvalues_length(moved) == 3); // x, y, vertical
		CHECK(fp3232(b[2]) - fp3232(a[2]) == 7.5);
	}
	CHECK(xi_event(&events, XCB_INPUT_BUTTON_PRESS, 0) == NULL); // half a notch
	CHECK(core_count(&core_events, XCB_MOTION_NOTIFY) == 0); // valuators aren't motion
	release(&events);
	release(&core_events);
	harness("scroll v 7.5");
	collect(c, &events);
	collect(core, &core_events);
	press = (void *)xi_event(&events, XCB_INPUT_BUTTON_PRESS, 0);
	CHECK(press && press->detail == 5 && (press->flags & XCB_INPUT_POINTER_EVENT_FLAGS_POINTER_EMULATED));
	CHECK(core_count(&core_events, XCB_BUTTON_PRESS) == 1);
	release(&events);
	release(&core_events);

	harness("motion 500 500");
	collect(c, &events);
	CHECK(xi_event(&events, XCB_INPUT_LEAVE, 0) != NULL);
	release(&events);
}

static void test_keys(xcb_connection_t *c, xcb_connection_t *core, xcb_screen_t *s) {
	xcb_window_t w = make_window(c, s, 0);
	uint32_t core_mask = XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE;
	xcb_change_window_attributes(core, w, XCB_CW_EVENT_MASK, &core_mask);
	free(xcb_get_input_focus_reply(core, xcb_get_input_focus(core), NULL));
	select_xi(c, w, XCB_INPUT_DEVICE_ALL_MASTER, XCB_INPUT_XI_EVENT_MASK_KEY_PRESS
		| XCB_INPUT_XI_EVENT_MASK_KEY_RELEASE | XCB_INPUT_XI_EVENT_MASK_FOCUS_IN);
	struct events events, core_events;
	collect(c, &events);
	release(&events);
	harness("focus %x", w);
	collect(c, &events);
	CHECK(xi_event(&events, XCB_INPUT_FOCUS_IN, 0) != NULL);
	release(&events);

	harness("key 38 1");
	collect(c, &events);
	xcb_input_key_press_event_t *key = (void *)xi_event(&events, XCB_INPUT_KEY_PRESS, 0);
	CHECK(key && key->detail == 38 && key->deviceid == 3 && key->sourceid == 5);
	release(&events);
	collect(core, &core_events);
	release(&core_events);

	// XI2: a press marked as a repeat. core: a release and a press
	harness("repeat 38");
	collect(c, &events);
	collect(core, &core_events);
	key = (void *)xi_event(&events, XCB_INPUT_KEY_PRESS, 0);
	CHECK(key && (key->flags & XCB_INPUT_KEY_EVENT_FLAGS_KEY_REPEAT));
	CHECK(xi_event(&events, XCB_INPUT_KEY_RELEASE, 0) == NULL);
	CHECK(core_count(&core_events, XCB_KEY_RELEASE) == 1);
	CHECK(core_count(&core_events, XCB_KEY_PRESS) == 1);
	release(&events);
	release(&core_events);
	harness("key 38 0");
	collect(c, &events);
	CHECK(xi_event(&events, XCB_INPUT_KEY_RELEASE, 0) != NULL);
	release(&events);
}

static void test_raw(xcb_connection_t *c, xcb_screen_t *s) {
	select_xi(c, s->root, XCB_INPUT_DEVICE_ALL_MASTER, XCB_INPUT_XI_EVENT_MASK_RAW_MOTION
		| XCB_INPUT_XI_EVENT_MASK_RAW_BUTTON_PRESS);
	struct events events;
	collect(c, &events);
	release(&events);
	harness("raw 3 -4.5");
	collect(c, &events);
	xcb_input_raw_motion_event_t *raw = (void *)xi_event(&events, XCB_INPUT_RAW_MOTION, 0);
	CHECK(raw != NULL);
	if (raw) {
		CHECK(xcb_input_raw_button_press_axisvalues_length(raw) == 2);
		xcb_input_fp3232_t *values = xcb_input_raw_button_press_axisvalues_raw(raw);
		CHECK(fp3232(values[0]) == 3 && fp3232(values[1]) == -4.5);
	}
	release(&events);
	harness("button 3 1");
	collect(c, &events);
	xcb_input_raw_button_press_event_t *rb = (void *)xi_event(&events,
		XCB_INPUT_RAW_BUTTON_PRESS, 0);
	CHECK(rb && rb->detail == 3);
	release(&events);
	harness("button 3 0");
}

static void test_randr(xcb_connection_t *c, xcb_screen_t *s) {
	xcb_randr_query_version_reply_t *v = xcb_randr_query_version_reply(c,
		xcb_randr_query_version(c, 1, 6), NULL);
	CHECK(v && v->major_version == 1 && v->minor_version == 5);
	free(v);
	xcb_randr_select_input(c, s->root, XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE
		| XCB_RANDR_NOTIFY_MASK_CRTC_CHANGE);
	struct events events;
	collect(c, &events);
	release(&events);
	harness("monitors 2");
	collect(c, &events);
	CHECK(core_count(&events, randr_event) == 1);
	CHECK(core_count(&events, randr_event + 1) == 2); // a crtc change each
	release(&events);

	xcb_randr_get_screen_resources_current_reply_t *r =
		xcb_randr_get_screen_resources_current_reply(c,
			xcb_randr_get_screen_resources_current(c, s->root), NULL);
	CHECK(r && r->num_outputs == 2 && r->num_crtcs == 2 && r->num_modes == 2);
	if (r) {
		xcb_randr_mode_info_t *mode = xcb_randr_get_screen_resources_current_modes(r);
		double hz = (double)mode->dot_clock / ((double)mode->htotal * mode->vtotal);
		CHECK(hz > 164.99 && hz < 165.01);
		xcb_randr_crtc_t *crtcs = xcb_randr_get_screen_resources_current_crtcs(r);
		xcb_randr_get_crtc_info_reply_t *ci = xcb_randr_get_crtc_info_reply(c,
			xcb_randr_get_crtc_info(c, crtcs[1], r->config_timestamp), NULL);
		CHECK(ci && ci->x == 1920 && ci->width == 1920 && ci->num_outputs == 1);
		free(ci);
		xcb_randr_output_t *outputs = xcb_randr_get_screen_resources_current_outputs(r);
		xcb_randr_get_output_info_reply_t *oi = xcb_randr_get_output_info_reply(c,
			xcb_randr_get_output_info(c, outputs[0], r->config_timestamp), NULL);
		CHECK(oi && oi->mm_width == 527 && oi->crtc == crtcs[0]);
		CHECK(oi && xcb_randr_get_output_info_name_length(oi) == 4
			&& !memcmp(xcb_randr_get_output_info_name(oi), "DP-1", 4));
		free(oi);
	}
	free(r);
	xcb_randr_get_monitors_reply_t *m = xcb_randr_get_monitors_reply(c,
		xcb_randr_get_monitors(c, s->root, 1), NULL);
	CHECK(m && m->nMonitors == 2);
	free(m);
	xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(c, xcb_get_geometry(c, s->root),
		NULL);
	CHECK(g && g->width == 3840 && g->height == 1080);
	free(g);
}

int main(void) {
	const char *fds = getenv("HARNESS_FDS");
	if (!fds || sscanf(fds, "%d %d", &to_harness, &from_harness) != 2) {
		fprintf(stderr, "run me with the harness\n");
		return 2;
	}
	xcb_connection_t *c = xcb_connect(NULL, NULL);
	xcb_connection_t *core = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(c) || xcb_connection_has_error(core)) {
		fprintf(stderr, "no X\n");
		return 2;
	}
	xcb_screen_t *s = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
	const xcb_query_extension_reply_t *xi = xcb_get_extension_data(c, &xcb_input_id);
	const xcb_query_extension_reply_t *rr = xcb_get_extension_data(c, &xcb_randr_id);
	CHECK(xi && xi->present);
	CHECK(rr && rr->present);
	if (!xi || !xi->present || !rr || !rr->present) {
		return 1;
	}
	xi_opcode = xi->major_opcode;
	randr_event = rr->first_event;

	test_devices(c);
	test_pointer(c, core, s);
	test_keys(c, core, s);
	test_raw(c, s);
	test_randr(c, s);

	xcb_disconnect(core);
	xcb_disconnect(c);
	if (failures) {
		fprintf(stderr, "xinput_test: %d failed\n", failures);
		return 1;
	}
	printf("xinput: all good\n");
	return 0;
}
