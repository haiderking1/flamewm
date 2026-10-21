// focus and crossing events against flame's X server, run by the harness:
// make test. the protocol's chains detail for detail (chromium works out
// whether it's active from them), the focus flag on crossings, and the
// window manager's focus ICCCM's way
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/xcb.h>

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

static xcb_connection_t *c;
static xcb_screen_t *s;

// what came in since last time, one letter word per event: "out T 2" is
// FocusOut on T with detail 2, "enter C 0 f" EnterNotify with the focus flag
#define MAX_SEEN 32
static char seen[MAX_SEEN][32];
static int seen_len;
static xcb_window_t names_id[8];
static char names[8];
static int names_len;

static char name_of(xcb_window_t w) {
	for (int i = 0; i < names_len; i++) {
		if (names_id[i] == w) {
			return names[i];
		}
	}
	return '?';
}

static void collect(void) {
	free(xcb_get_input_focus_reply(c, xcb_get_input_focus(c), NULL)); // a round trip
	seen_len = 0;
	xcb_generic_event_t *e;
	while ((e = xcb_poll_for_event(c))) {
		uint8_t type = e->response_type & 0x7f;
		char *line = seen_len < MAX_SEEN ? seen[seen_len] : NULL;
		if (line && (type == XCB_FOCUS_IN || type == XCB_FOCUS_OUT)) {
			xcb_focus_in_event_t *f = (xcb_focus_in_event_t *)e;
			snprintf(line, 32, "%s %c %d", type == XCB_FOCUS_IN ? "in" : "out",
				name_of(f->event), f->detail);
			seen_len++;
		} else if (line && (type == XCB_ENTER_NOTIFY || type == XCB_LEAVE_NOTIFY)) {
			xcb_enter_notify_event_t *n = (xcb_enter_notify_event_t *)e;
			snprintf(line, 32, "%s %c %d%s", type == XCB_ENTER_NOTIFY ? "enter" : "leave",
				name_of(n->event), n->detail, n->same_screen_focus & 1 ? " f" : "");
			seen_len++;
		} else if (line && type == XCB_CLIENT_MESSAGE) {
			snprintf(line, 32, "message %c", name_of(((xcb_client_message_event_t *)e)->window));
			seen_len++;
		}
		free(e);
	}
}

// exactly these, in this order
static bool got(int n, ...) {
	va_list args;
	va_start(args, n);
	bool ok = n == seen_len;
	for (int i = 0; i < n; i++) {
		const char *want = va_arg(args, const char *);
		if (i >= seen_len || strcmp(want, seen[i])) {
			ok = false;
		}
	}
	va_end(args);
	if (!ok) {
		fprintf(stderr, "  saw:");
		for (int i = 0; i < seen_len; i++) {
			fprintf(stderr, " [%s]", seen[i]);
		}
		fprintf(stderr, "\n");
	}
	return ok;
}

static xcb_window_t window(char name, xcb_window_t parent, int x, int y, int w, int h) {
	xcb_window_t id = xcb_generate_id(c);
	uint32_t mask = XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_ENTER_WINDOW
		| XCB_EVENT_MASK_LEAVE_WINDOW;
	xcb_create_window(c, XCB_COPY_FROM_PARENT, id, parent, (int16_t)x, (int16_t)y,
		(uint16_t)w, (uint16_t)h, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, s->root_visual,
		XCB_CW_EVENT_MASK, &mask);
	xcb_map_window(c, id);
	names_id[names_len] = id;
	names[names_len++] = name;
	return id;
}

static xcb_atom_t atom(const char *name) {
	xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(c,
		xcb_intern_atom(c, 0, (uint16_t)strlen(name), name), NULL);
	xcb_atom_t a = r ? r->atom : XCB_NONE;
	free(r);
	return a;
}

static xcb_window_t focus(void) {
	xcb_get_input_focus_reply_t *r = xcb_get_input_focus_reply(c, xcb_get_input_focus(c),
		NULL);
	xcb_window_t w = r ? r->focus : 0;
	free(r);
	return w;
}

int main(void) {
	const char *fds = getenv("HARNESS_FDS");
	if (!fds || sscanf(fds, "%d %d", &to_harness, &from_harness) != 2) {
		fprintf(stderr, "run me with the harness\n");
		return 2;
	}
	c = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(c)) {
		return 2;
	}
	s = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
	// T a top-level, C in it, D in C. U another top-level
	xcb_window_t t = window('T', s->root, 0, 0, 400, 300);
	xcb_window_t ch = window('C', t, 50, 50, 100, 100);
	xcb_window_t d = window('D', ch, 10, 10, 50, 50);
	xcb_window_t u = window('U', s->root, 500, 0, 200, 200);
	collect();

	// focus: the window manager on T, then the app moving it inside
	harness("focus %x", t);
	collect();
	CHECK(got(1, "in T 3")); // Nonlinear
	xcb_set_input_focus(c, XCB_INPUT_FOCUS_PARENT, d, XCB_CURRENT_TIME);
	collect();
	// T's focus went into its own child: Inferior, chromium keeps T active
	CHECK(got(3, "out T 2", "in C 1", "in D 0"));
	xcb_set_input_focus(c, XCB_INPUT_FOCUS_PARENT, t, XCB_CURRENT_TIME);
	collect();
	CHECK(got(3, "out D 0", "out C 1", "in T 2"));

	// crossings, focus on T: the flag on T and everything in it
	harness("motion 20 20");
	collect();
	CHECK(got(1, "enter T 0 f")); // from the root: Ancestor
	harness("motion 55 55");
	collect();
	CHECK(got(2, "leave T 2 f", "enter C 0 f"));
	harness("motion 70 70");
	collect();
	CHECK(got(2, "leave C 2 f", "enter D 0 f"));
	harness("motion 20 20");
	collect();
	CHECK(got(3, "leave D 0 f", "leave C 1 f", "enter T 2 f"));
	harness("motion 70 70");
	collect();
	CHECK(got(3, "leave T 2 f", "enter C 1 f", "enter D 0 f"));
	harness("motion 550 50");
	collect();
	// D to U: nonlinear, C and T virtual on the way out
	CHECK(got(4, "leave D 3 f", "leave C 4 f", "leave T 4 f", "enter U 3"));

	// focus out of T with the pointer on U: Nonlinear both ends
	harness("focus %x", u);
	collect();
	CHECK(got(2, "out T 3", "in U 3"));

	// the window manager's focus: already in T (on D, the app's choice), it
	// stays where it is
	xcb_set_input_focus(c, XCB_INPUT_FOCUS_PARENT, d, XCB_CURRENT_TIME);
	collect();
	harness("wmfocus %x", t);
	CHECK(focus() == d);
	// a window that takes no input but WM_TAKE_FOCUS: asked, not focused
	xcb_window_t g = window('G', s->root, 0, 400, 50, 50);
	uint32_t hints[9] = {1, 0}; // InputHint, input false
	xcb_change_property(c, XCB_PROP_MODE_REPLACE, g, XCB_ATOM_WM_HINTS, XCB_ATOM_WM_HINTS, 32,
		9, hints);
	xcb_atom_t take = atom("WM_TAKE_FOCUS");
	xcb_change_property(c, XCB_PROP_MODE_REPLACE, g, atom("WM_PROTOCOLS"), XCB_ATOM_ATOM, 32,
		1, &take);
	collect();
	harness("wmfocus %x", g);
	collect();
	CHECK(focus() == d);
	CHECK(got(1, "message G"));
	// plain: focused
	harness("wmfocus %x", u);
	CHECK(focus() == u);

	xcb_disconnect(c);
	if (failures) {
		fprintf(stderr, "focus_test: %d failed\n", failures);
		return 1;
	}
	printf("focus: all good\n");
	return 0;
}
