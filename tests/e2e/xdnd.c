// X drag and drop test client (XDND 5)
//   xdnd source TEXT [secs]   a window; press in it and drag TEXT (UTF8_STRING, text/plain)
//   xdnd target [secs]        a window; takes text drops, prints "dropped: TEXT", exits
//   xdnd popup [secs]         an override-redirect window, like a menu
//   xdnd dialog [secs]        a dialog window (_NET_WM_WINDOW_TYPE_DIALOG), 400x300
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xcb/xcb.h>

static xcb_connection_t *c;
static xcb_screen_t *s;
static xcb_window_t w;
static const char *mode, *text;
static xcb_atom_t aware, enter, position, status, leave, drop, finished, selection, copy;
static xcb_atom_t utf8, plain, prop, targets;

static xcb_atom_t atom(const char *name) {
	xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, strlen(name), name), NULL);
	xcb_atom_t a = r ? r->atom : 0;
	free(r);
	return a;
}

static void message(xcb_window_t to, xcb_atom_t type, uint32_t a, uint32_t b, uint32_t cc, uint32_t d, uint32_t e) {
	xcb_client_message_event_t ev = {0};
	ev.response_type = XCB_CLIENT_MESSAGE;
	ev.format = 32;
	ev.window = to;
	ev.type = type;
	ev.data.data32[0] = a; ev.data.data32[1] = b; ev.data.data32[2] = cc;
	ev.data.data32[3] = d; ev.data.data32[4] = e;
	xcb_send_event(c, 0, to, 0, (const char *)&ev);
	xcb_flush(c);
}

static void say(const char *fmt, const char *arg) {
	printf("%s: ", mode);
	printf(fmt, arg);
	printf("\n");
	fflush(stdout);
}

// source state
static bool dragging, accepted;
static xcb_window_t current;

static xcb_window_t aware_window_at(int16_t *rx, int16_t *ry) {
	xcb_query_pointer_reply_t *p = xcb_query_pointer_reply(c, xcb_query_pointer(c, s->root), NULL);
	if (!p) return 0;
	xcb_window_t child = p->child;
	*rx = p->root_x; *ry = p->root_y;
	free(p);
	if (!child) return 0;
	xcb_get_property_reply_t *r = xcb_get_property_reply(c, xcb_get_property(c, 0, child, aware, XCB_ATOM_ATOM, 0, 1), NULL);
	bool is = r && xcb_get_property_value_length(r) == 4;
	free(r);
	return is ? child : 0;
}

int main(int argc, char **argv) {
	mode = argc > 1 ? argv[1] : "target";
	text = argc > 2 && !strcmp(mode, "source") ? argv[2] : "";
	int secs = atoi(argc > 3 ? argv[3] : argc > 2 && strcmp(mode, "source") ? argv[2] : "15");
	c = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(c)) { fprintf(stderr, "no X\n"); return 2; }
	s = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
	aware = atom("XdndAware"); enter = atom("XdndEnter"); position = atom("XdndPosition");
	status = atom("XdndStatus"); leave = atom("XdndLeave"); drop = atom("XdndDrop");
	finished = atom("XdndFinished"); selection = atom("XdndSelection"); copy = atom("XdndActionCopy");
	utf8 = atom("UTF8_STRING"); plain = atom("text/plain"); prop = atom("XDND_DATA"); targets = atom("TARGETS");
	w = xcb_generate_id(c);
	uint32_t values[] = {!strcmp(mode, "source") ? 0x2060c0u : 0x20a040u,
		XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION
		| XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_PROPERTY_CHANGE};
	bool popup = !strcmp(mode, "popup");
	uint32_t popup_values[] = {values[0], 1, values[1]};
	xcb_create_window(c, 24, w, s->root, popup ? 100 : 0, popup ? 100 : 0, 400, 300, 0,
		XCB_WINDOW_CLASS_INPUT_OUTPUT, s->root_visual, popup
		? XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK
		: XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, popup ? popup_values : values);
	xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, strlen(mode), mode);
	if (!strcmp(mode, "dialog")) {
		xcb_atom_t type = atom("_NET_WM_WINDOW_TYPE"), dialog = atom("_NET_WM_WINDOW_TYPE_DIALOG");
		xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, type, XCB_ATOM_ATOM, 32, 1, &dialog);
	}
	if (!strcmp(mode, "target")) {
		uint32_t version = 5;
		xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, aware, XCB_ATOM_ATOM, 32, 1, &version);
	}
	xcb_map_window(c, w);
	xcb_flush(c);
	time_t end = time(NULL) + secs;
	xcb_window_t drag_source = 0;
	while (time(NULL) < end) {
		struct pollfd p = {xcb_get_file_descriptor(c), POLLIN, 0};
		poll(&p, 1, 100);
		xcb_generic_event_t *e;
		while ((e = xcb_poll_for_event(c))) {
			uint8_t t = e->response_type & 0x7f;
			if (t == XCB_EXPOSE) {
				// clear so it shows
				xcb_clear_area(c, 0, w, 0, 0, 0, 0);
				xcb_flush(c);
			} else if (!strcmp(mode, "source") && t == XCB_BUTTON_PRESS) {
				dragging = true;
				xcb_set_selection_owner(c, w, selection, XCB_CURRENT_TIME);
				xcb_flush(c);
				say("drag started%s", "");
			} else if (!strcmp(mode, "source") && t == XCB_MOTION_NOTIFY && dragging) {
				int16_t rx, ry;
				xcb_window_t target = aware_window_at(&rx, &ry);
				if (target != current) {
					if (current) message(current, leave, w, 0, 0, 0, 0);
					current = target;
					accepted = false;
					if (current) {
						char id[32]; snprintf(id, sizeof(id), "0x%x", current);
						say("enter %s", id);
						message(current, enter, w, 5u << 24, utf8, plain, 0);
					}
				}
				if (current) message(current, position, w, 0, (uint32_t)rx << 16 | (uint16_t)ry, XCB_CURRENT_TIME, copy);
			} else if (!strcmp(mode, "source") && t == XCB_BUTTON_RELEASE && dragging) {
				dragging = false;
				if (current && accepted) {
					say("drop%s", "");
					message(current, drop, w, 0, XCB_CURRENT_TIME, 0, 0);
				} else {
					if (current) message(current, leave, w, 0, 0, 0, 0);
					say("cancelled%s", "");
					current = 0;
				}
			} else if (t == XCB_SELECTION_REQUEST) {
				xcb_selection_request_event_t *r = (void *)e;
				xcb_selection_notify_event_t n = {XCB_SELECTION_NOTIFY, 0, 0, r->time, r->requestor, r->selection, r->target, r->property};
				if (r->target == targets) {
					xcb_atom_t list[] = {targets, utf8, plain};
					xcb_change_property(c, XCB_PROP_MODE_REPLACE, r->requestor, r->property, XCB_ATOM_ATOM, 32, 3, list);
				} else if (r->target == utf8 || r->target == plain) {
					xcb_change_property(c, XCB_PROP_MODE_REPLACE, r->requestor, r->property, r->target, 8, strlen(text), text);
					char *name = r->target == utf8 ? "UTF8_STRING" : "text/plain";
					say("sent %s", name);
				} else {
					n.property = 0;
				}
				xcb_send_event(c, 0, r->requestor, 0, (const char *)&n);
				xcb_flush(c);
			} else if (t == XCB_CLIENT_MESSAGE) {
				xcb_client_message_event_t *m = (void *)e;
				uint32_t *d = m->data.data32;
				if (m->type == status) {
					accepted = d[1] & 1;
				} else if (m->type == finished) {
					say(d[1] & 1 ? "finished, accepted%s" : "finished, refused%s", "");
					free(e);
					return 0;
				} else if (m->type == enter) {
					drag_source = d[0];
					char id[32]; snprintf(id, sizeof(id), "0x%x", drag_source);
					say("enter from %s", id);
				} else if (m->type == position) {
					message(d[0], status, w, 1 | 2, 0, 0, copy);
				} else if (m->type == leave) {
					say("leave%s", "");
				} else if (m->type == drop) {
					xcb_convert_selection(c, w, selection, utf8, prop, d[2]);
					xcb_flush(c);
				}
			} else if (t == XCB_SELECTION_NOTIFY && !strcmp(mode, "target")) {
				xcb_selection_notify_event_t *n = (void *)e;
				char buf[512] = "";
				if (n->property) {
					xcb_get_property_reply_t *r = xcb_get_property_reply(c, xcb_get_property(c, 1, w, prop, XCB_GET_PROPERTY_TYPE_ANY, 0, 128), NULL);
					if (r) snprintf(buf, sizeof(buf), "%.*s", xcb_get_property_value_length(r), (char *)xcb_get_property_value(r));
					free(r);
				}
				printf("dropped: %s\n", buf);
				fflush(stdout);
				message(drag_source, finished, w, n->property ? 1 : 0, copy, 0, 0);
				free(e);
				return 0;
			}
			free(e);
		}
	}
	return 1;
}
