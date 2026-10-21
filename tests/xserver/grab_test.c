// active grabs against flame's X server, run by the harness: make test. a
// client holding the pointer or the keyboard (a menu open) gets everything,
// the others nothing, crossings say Grab and Ungrab, XI2 grabs too
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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

// what a connection got since last time: "press 1 x,y", "enter mode",
// "leave mode", "key", "xi-press device"
struct seen {
	char lines[16][32];
	int len;
};

static void collect(xcb_connection_t *c, struct seen *seen, uint8_t xi_opcode) {
	free(xcb_get_input_focus_reply(c, xcb_get_input_focus(c), NULL));
	seen->len = 0;
	xcb_generic_event_t *e;
	while ((e = xcb_poll_for_event(c))) {
		uint8_t type = e->response_type & 0x7f;
		char *line = seen->len < 16 ? seen->lines[seen->len] : NULL;
		if (!line) {
			free(e);
			continue;
		}
		if (type == XCB_BUTTON_PRESS) {
			xcb_button_press_event_t *b = (xcb_button_press_event_t *)e;
			snprintf(line, 32, "press %d %d,%d", b->detail, b->event_x, b->event_y);
			seen->len++;
		} else if (type == XCB_ENTER_NOTIFY || type == XCB_LEAVE_NOTIFY) {
			xcb_enter_notify_event_t *n = (xcb_enter_notify_event_t *)e;
			snprintf(line, 32, "%s %d", type == XCB_ENTER_NOTIFY ? "enter" : "leave", n->mode);
			seen->len++;
		} else if (type == XCB_KEY_PRESS) {
			snprintf(line, 32, "key");
			seen->len++;
		} else if (type == XCB_GE_GENERIC) {
			xcb_ge_generic_event_t *g = (xcb_ge_generic_event_t *)e;
			if (g->extension == xi_opcode && g->event_type == XCB_INPUT_BUTTON_PRESS) {
				xcb_input_button_press_event_t *b = (xcb_input_button_press_event_t *)e;
				snprintf(line, 32, "xi-press %d", b->deviceid);
				seen->len++;
			}
		}
		free(e);
	}
}

static bool has(const struct seen *seen, const char *want) {
	for (int i = 0; i < seen->len; i++) {
		if (!strcmp(seen->lines[i], want)) {
			return true;
		}
	}
	fprintf(stderr, "  wanted [%s], saw:", want);
	for (int i = 0; i < seen->len; i++) {
		fprintf(stderr, " [%s]", seen->lines[i]);
	}
	fprintf(stderr, "\n");
	return false;
}

static xcb_window_t window(xcb_connection_t *c, xcb_screen_t *s, int x, int y) {
	xcb_window_t id = xcb_generate_id(c);
	uint32_t mask = XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_KEY_PRESS
		| XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW;
	xcb_create_window(c, XCB_COPY_FROM_PARENT, id, s->root, (int16_t)x, (int16_t)y, 100, 100,
		0, XCB_WINDOW_CLASS_INPUT_OUTPUT, s->root_visual, XCB_CW_EVENT_MASK, &mask);
	xcb_map_window(c, id);
	free(xcb_get_input_focus_reply(c, xcb_get_input_focus(c), NULL));
	return id;
}

int main(void) {
	const char *fds = getenv("HARNESS_FDS");
	if (!fds || sscanf(fds, "%d %d", &to_harness, &from_harness) != 2) {
		fprintf(stderr, "run me with the harness\n");
		return 2;
	}
	xcb_connection_t *a = xcb_connect(NULL, NULL), *b = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(a) || xcb_connection_has_error(b)) {
		return 2;
	}
	xcb_screen_t *s = xcb_setup_roots_iterator(xcb_get_setup(a)).data;
	const xcb_query_extension_reply_t *xi = xcb_get_extension_data(a, &xcb_input_id);
	uint8_t xi_opcode = xi ? xi->major_opcode : 0;
	free(xcb_input_xi_query_version_reply(a, xcb_input_xi_query_version(a, 2, 2), NULL));
	// G: A's menu at 0,0. W: B's window at 200,0
	xcb_window_t g = window(a, s, 0, 0);
	xcb_window_t w = window(b, s, 200, 0);
	struct seen sa, sb;
	harness("motion 250 50");
	collect(a, &sa, xi_opcode);
	collect(b, &sb, xi_opcode);

	// A grabs the pointer: Enter on G as a grab, B's clicks are A's now,
	// where they are relative to G
	xcb_grab_pointer_reply_t *r = xcb_grab_pointer_reply(a, xcb_grab_pointer(a, 0, g,
		XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW,
		XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE, XCB_CURRENT_TIME), NULL);
	CHECK(r && r->status == XCB_GRAB_STATUS_SUCCESS);
	free(r);
	collect(a, &sa, xi_opcode);
	CHECK(has(&sa, "enter 1"));
	r = xcb_grab_pointer_reply(b, xcb_grab_pointer(b, 0, w, 0, XCB_GRAB_MODE_ASYNC,
		XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE, XCB_CURRENT_TIME), NULL);
	CHECK(r && r->status == XCB_GRAB_STATUS_ALREADY_GRABBED);
	free(r);
	harness("button 1 1");
	harness("button 1 0");
	collect(a, &sa, xi_opcode);
	collect(b, &sb, xi_opcode);
	CHECK(has(&sa, "press 1 250,50"));
	CHECK(sb.len == 0);
	// over nothing of X's at all: still A's
	harness("motion 900 900");
	harness("button 1 1");
	harness("button 1 0");
	collect(a, &sa, xi_opcode);
	CHECK(has(&sa, "press 1 900,900"));
	harness("motion 250 50");
	collect(a, &sa, xi_opcode);
	collect(b, &sb, xi_opcode);

	// let go: Leave on G as an ungrab, B's clicks are B's again
	xcb_ungrab_pointer(a, XCB_CURRENT_TIME);
	collect(a, &sa, xi_opcode);
	CHECK(has(&sa, "leave 2"));
	harness("button 1 1");
	harness("button 1 0");
	collect(a, &sa, xi_opcode);
	collect(b, &sb, xi_opcode);
	CHECK(has(&sb, "press 1 50,50"));
	CHECK(sa.len == 0);

	// the keyboard: focus on W, A holding it gets the keys
	harness("focus %x", w);
	xcb_grab_keyboard_reply_t *k = xcb_grab_keyboard_reply(a, xcb_grab_keyboard(a, 0, g,
		XCB_CURRENT_TIME, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC), NULL);
	CHECK(k && k->status == XCB_GRAB_STATUS_SUCCESS);
	free(k);
	collect(a, &sa, xi_opcode);
	collect(b, &sb, xi_opcode);
	harness("key 38 1");
	harness("key 38 0");
	collect(a, &sa, xi_opcode);
	collect(b, &sb, xi_opcode);
	CHECK(has(&sa, "key"));
	CHECK(sb.len == 0);
	xcb_ungrab_keyboard(a, XCB_CURRENT_TIME);

	// XI2: the master pointer, ButtonPress in the mask
	uint32_t mask = XCB_INPUT_XI_EVENT_MASK_BUTTON_PRESS;
	xcb_input_xi_grab_device_reply_t *x = xcb_input_xi_grab_device_reply(a,
		xcb_input_xi_grab_device(a, g, XCB_CURRENT_TIME, XCB_NONE, 2,
			XCB_INPUT_GRAB_MODE_22_ASYNC, XCB_INPUT_GRAB_MODE_22_ASYNC,
			XCB_INPUT_GRAB_OWNER_NO_OWNER, 1, &mask), NULL);
	CHECK(x && x->status == XCB_GRAB_STATUS_SUCCESS);
	free(x);
	collect(a, &sa, xi_opcode);
	harness("button 1 1");
	harness("button 1 0");
	collect(a, &sa, xi_opcode);
	collect(b, &sb, xi_opcode);
	CHECK(has(&sa, "xi-press 2"));
	CHECK(sb.len == 0);

	// the grab window goes out of sight: the grab's gone with it
	xcb_unmap_window(a, g);
	collect(a, &sa, xi_opcode);
	harness("button 1 1");
	harness("button 1 0");
	collect(b, &sb, xi_opcode);
	CHECK(has(&sb, "press 1 50,50"));

	xcb_disconnect(a);
	xcb_disconnect(b);
	if (failures) {
		fprintf(stderr, "grab_test: %d failed\n", failures);
		return 1;
	}
	printf("grab: all good\n");
	return 0;
}
