// selections between X clients and flame (the clipboard bridge's X side),
// against flame's X server, run by the harness: make test
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/xfixes.h>

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

#define FLAME_WINDOW 0x102
#define CHUNK 4096

static int to_harness, from_harness;
static xcb_connection_t *c;
static xcb_window_t w;
static xcb_atom_t clipboard, targets, utf8, incr, prop;

// what this client serves while it owns the clipboard
static const char *offer;
static size_t offer_len;
static xcb_window_t incr_to;
static xcb_atom_t incr_prop;
static size_t incr_at;
static int clears, client_closes;
static uint8_t xfixes_event;

static xcb_atom_t atom(const char *name) {
	xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(c,
		xcb_intern_atom(c, 0, (uint16_t)strlen(name), name), NULL);
	xcb_atom_t a = r ? r->atom : 0;
	free(r);
	return a;
}

// the owner's side: TARGETS and UTF8_STRING, INCR when it's big
static void serve(xcb_generic_event_t *e) {
	uint8_t type = e->response_type & 0x7f;
	if (type == XCB_SELECTION_CLEAR) {
		clears++;
	} else if (xfixes_event && type == xfixes_event) {
		xcb_xfixes_selection_notify_event_t *n = (void *)e;
		client_closes += n->subtype == XCB_XFIXES_SELECTION_EVENT_SELECTION_CLIENT_CLOSE;
	} else if (type == XCB_SELECTION_REQUEST) {
		xcb_selection_request_event_t *r = (void *)e;
		xcb_selection_notify_event_t n = {XCB_SELECTION_NOTIFY, 0, 0, r->time, r->requestor,
			r->selection, r->target, r->property};
		if (r->target == targets) {
			xcb_atom_t list[] = {targets, utf8};
			xcb_change_property(c, XCB_PROP_MODE_REPLACE, r->requestor, r->property,
				XCB_ATOM_ATOM, 32, 2, list);
		} else if (r->target == utf8 && offer_len > CHUNK) {
			uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE, size = (uint32_t)offer_len;
			xcb_change_window_attributes(c, r->requestor, XCB_CW_EVENT_MASK, &mask);
			xcb_change_property(c, XCB_PROP_MODE_REPLACE, r->requestor, r->property, incr, 32, 1,
				&size);
			incr_to = r->requestor;
			incr_prop = r->property;
			incr_at = 0;
		} else if (r->target == utf8) {
			xcb_change_property(c, XCB_PROP_MODE_REPLACE, r->requestor, r->property, utf8, 8,
				(uint32_t)offer_len, offer);
		} else {
			n.property = 0;
		}
		xcb_send_event(c, 0, r->requestor, 0, (const char *)&n);
	} else if (type == XCB_PROPERTY_NOTIFY) {
		xcb_property_notify_event_t *pn = (void *)e;
		if (incr_to && pn->window == incr_to && pn->atom == incr_prop
				&& pn->state == XCB_PROPERTY_DELETE) {
			size_t n = offer_len - incr_at < CHUNK ? offer_len - incr_at : CHUNK;
			xcb_change_property(c, XCB_PROP_MODE_REPLACE, incr_to, incr_prop, utf8, 8,
				(uint32_t)n, offer + incr_at);
			incr_at += n;
			incr_to = n ? incr_to : 0;
		}
	}
	xcb_flush(c);
}

// a harness command, serving X events until its line comes back
static void harness(char *answer, size_t size, const char *fmt, ...) {
	char line[128];
	va_list args;
	va_start(args, fmt);
	int len = vsnprintf(line, sizeof(line) - 1, fmt, args);
	va_end(args);
	line[len++] = '\n';
	if (write(to_harness, line, (size_t)len) != len) {
		exit(3);
	}
	size_t got = 0;
	for (;;) {
		struct pollfd fds[2] = {{from_harness, POLLIN, 0}, {xcb_get_file_descriptor(c), POLLIN, 0}};
		poll(fds, 2, 1000);
		xcb_generic_event_t *e;
		while ((e = xcb_poll_for_event(c))) {
			serve(e);
			free(e);
		}
		if (fds[0].revents & (POLLIN | POLLHUP)) {
			char ch;
			if (read(from_harness, &ch, 1) != 1) {
				exit(3);
			}
			if (ch == '\n') {
				if (answer) {
					answer[got < size ? got : size - 1] = '\0';
				}
				return;
			}
			if (answer && got + 1 < size) {
				answer[got] = ch;
			}
			got++;
		}
	}
}

static void sync_x(void) {
	free(xcb_get_input_focus_reply(c, xcb_get_input_focus(c), NULL));
}

// ConvertSelection the way apps do it, the answer from the property
static bool paste(char *out, size_t size) {
	xcb_convert_selection(c, w, clipboard, utf8, prop, XCB_CURRENT_TIME);
	xcb_flush(c);
	for (int i = 0; i < 100; i++) {
		xcb_generic_event_t *e = xcb_poll_for_event(c);
		if (!e) {
			struct pollfd p = {xcb_get_file_descriptor(c), POLLIN, 0};
			poll(&p, 1, 20);
			continue;
		}
		uint8_t type = e->response_type & 0x7f;
		bool notify = type == XCB_SELECTION_NOTIFY;
		bool got = notify && ((xcb_selection_notify_event_t *)e)->property != 0;
		free(e);
		if (!notify) {
			continue;
		}
		if (!got) {
			return false;
		}
		xcb_get_property_reply_t *r = xcb_get_property_reply(c, xcb_get_property(c, 1, w, prop,
			XCB_GET_PROPERTY_TYPE_ANY, 0, 1024), NULL);
		int len = r ? xcb_get_property_value_length(r) : 0;
		snprintf(out, size, "%.*s", len, r ? (char *)xcb_get_property_value(r) : "");
		free(r);
		return true;
	}
	return false;
}

static void test_flame_owns(void) {
	harness(NULL, 0, "own CLIPBOARD");
	xcb_get_selection_owner_reply_t *o = xcb_get_selection_owner_reply(c,
		xcb_get_selection_owner(c, clipboard), NULL);
	CHECK(o && o->owner == FLAME_WINDOW);
	free(o);
	char text[64] = "";
	CHECK(paste(text, sizeof(text)));
	CHECK(!strcmp(text, "flame clipboard"));
}

static void test_x_owns(void) {
	char answer[256];
	harness(answer, sizeof(answer), "owner CLIPBOARD");
	offer = "from an X app";
	offer_len = strlen(offer);
	clears = 0;
	xcb_set_selection_owner(c, w, clipboard, XCB_CURRENT_TIME);
	sync_x();
	harness(answer, sizeof(answer), "owner CLIPBOARD");
	CHECK(!strcmp(answer, "ok 1 CLIPBOARD=1"));
	harness(answer, sizeof(answer), "convert CLIPBOARD TARGETS");
	CHECK(!strncmp(answer, "ok ATOM 8 ", 10));
	harness(answer, sizeof(answer), "convert CLIPBOARD UTF8_STRING");
	CHECK(!strcmp(answer, "ok UTF8_STRING 13 from an X app"));
	harness(answer, sizeof(answer), "convert CLIPBOARD image/png");
	CHECK(!strcmp(answer, "ok None 0 "));

	// big: it comes in pieces, all of it
	static char big[100000];
	for (size_t i = 0; i < sizeof(big); i++) {
		big[i] = (char)('a' + i % 26);
	}
	offer = big;
	offer_len = sizeof(big);
	harness(answer, sizeof(answer), "convert CLIPBOARD UTF8_STRING");
	CHECK(!strcmp(answer, "ok UTF8_STRING 100000 abcdefghijklmnopqrstuvwxyzabcdef"));

	// flame takes it back: the X owner hears
	harness(NULL, 0, "own CLIPBOARD");
	sync_x();
	xcb_generic_event_t *e;
	while ((e = xcb_poll_for_event(c))) {
		serve(e);
		free(e);
	}
	CHECK(clears == 1);
}

static void test_owner_goes(void) {
	char answer[256];
	xcb_connection_t *other = xcb_connect(NULL, NULL);
	xcb_screen_t *s = xcb_setup_roots_iterator(xcb_get_setup(other)).data;
	xcb_window_t ow = xcb_generate_id(other);
	xcb_create_window(other, 0, ow, s->root, 0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY, 0, 0,
		NULL);
	xcb_set_selection_owner(other, ow, clipboard, XCB_CURRENT_TIME);
	free(xcb_get_input_focus_reply(other, xcb_get_input_focus(other), NULL));
	// XFIXES: this client hears owners change
	xcb_xfixes_query_version_reply_t *v = xcb_xfixes_query_version_reply(c,
		xcb_xfixes_query_version(c, 5, 0), NULL);
	free(v);
	const xcb_query_extension_reply_t *ext = xcb_get_extension_data(c, &xcb_xfixes_id);
	xfixes_event = ext ? (uint8_t)(ext->first_event + XCB_XFIXES_SELECTION_NOTIFY) : 0;
	xcb_xfixes_select_selection_input(c, w, clipboard,
		XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER
		| XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE);
	sync_x();
	harness(answer, sizeof(answer), "owner CLIPBOARD");
	CHECK(!strncmp(answer, "ok 1", 4));
	xcb_disconnect(other);
	nanosleep(&(struct timespec){0, 100000000}, NULL);
	harness(answer, sizeof(answer), "owner CLIPBOARD");
	CHECK(!strcmp(answer, "ok 0 CLIPBOARD=0"));
	sync_x();
	xcb_generic_event_t *e;
	while ((e = xcb_poll_for_event(c))) {
		serve(e);
		free(e);
	}
	CHECK(client_closes == 1);
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
	xcb_screen_t *s = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
	w = xcb_generate_id(c);
	uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
	xcb_create_window(c, 0, w, s->root, 0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY, 0,
		XCB_CW_EVENT_MASK, &mask);
	clipboard = atom("CLIPBOARD");
	targets = atom("TARGETS");
	utf8 = atom("UTF8_STRING");
	incr = atom("INCR");
	prop = atom("PASTE");

	test_flame_owns();
	test_x_owns();
	test_owner_goes();

	xcb_disconnect(c);
	if (failures) {
		fprintf(stderr, "selection_test: %d failed\n", failures);
		return 1;
	}
	printf("selection: all good\n");
	return 0;
}
