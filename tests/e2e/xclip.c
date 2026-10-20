// tiny X clipboard tool for the e2e tests: xclip copy FILE [seconds] | xclip paste [TARGET] | xclip targets
// copy serves CLIPBOARD (TARGETS, UTF8_STRING, image/png) from FILE, INCR when big
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <poll.h>
#include <xcb/xcb.h>

static xcb_connection_t *c;
static xcb_window_t w;

static xcb_atom_t atom(const char *name) {
	xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, strlen(name), name), NULL);
	xcb_atom_t a = r ? r->atom : 0;
	free(r);
	return a;
}

static char *slurp(const char *path, size_t *len) {
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	*len = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	char *b = malloc(*len + 1);
	if (fread(b, 1, *len, f) != *len) { fclose(f); free(b); return NULL; }
	fclose(f);
	return b;
}

#define CHUNK 65536

int main(int argc, char **argv) {
	if (argc < 2) return 2;
	c = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(c)) { fprintf(stderr, "no X\n"); return 2; }
	xcb_screen_t *s = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
	w = xcb_generate_id(c);
	uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
	xcb_create_window(c, 0, w, s->root, 0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY, 0, XCB_CW_EVENT_MASK, &mask);
	xcb_atom_t clipboard = atom("CLIPBOARD"), targets = atom("TARGETS"), utf8 = atom("UTF8_STRING");
	xcb_atom_t incr = atom("INCR"), prop = atom("XCLIPT"), png = atom("image/png");
	if (!strcmp(argv[1], "copy")) {
		size_t len;
		char *data = slurp(argv[2], &len);
		if (!data) return 2;
		int seconds = argc > 3 ? atoi(argv[3]) : 10;
		xcb_set_selection_owner(c, w, clipboard, XCB_CURRENT_TIME);
		xcb_flush(c);
		time_t end = time(NULL) + seconds;
		// one INCR transfer at a time is plenty here
		xcb_window_t incr_to = 0; xcb_atom_t incr_prop = 0, incr_type = 0; size_t incr_at = 0;
		while (time(NULL) < end) {
			struct pollfd p = {xcb_get_file_descriptor(c), POLLIN, 0};
			poll(&p, 1, 100);
			xcb_generic_event_t *e;
			while ((e = xcb_poll_for_event(c))) {
				uint8_t t = e->response_type & 0x7f;
				if (t == XCB_SELECTION_REQUEST) {
					xcb_selection_request_event_t *r = (void *)e;
					xcb_selection_notify_event_t n = {XCB_SELECTION_NOTIFY, 0, 0, r->time, r->requestor, r->selection, r->target, r->property};
					if (r->target == targets) {
						xcb_atom_t list[] = {targets, utf8, png};
						xcb_change_property(c, XCB_PROP_MODE_REPLACE, r->requestor, r->property, XCB_ATOM_ATOM, 32, 3, list);
					} else if (r->target == utf8 || r->target == png) {
						if (len > CHUNK) {
							uint32_t size = (uint32_t)len;
							uint32_t m = XCB_EVENT_MASK_PROPERTY_CHANGE;
							xcb_change_window_attributes(c, r->requestor, XCB_CW_EVENT_MASK, &m);
							xcb_change_property(c, XCB_PROP_MODE_REPLACE, r->requestor, r->property, incr, 32, 1, &size);
							incr_to = r->requestor; incr_prop = r->property; incr_type = r->target; incr_at = 0;
						} else {
							xcb_change_property(c, XCB_PROP_MODE_REPLACE, r->requestor, r->property, r->target, 8, (uint32_t)len, data);
						}
					} else {
						n.property = 0;
					}
					xcb_send_event(c, 0, r->requestor, 0, (const char *)&n);
					xcb_flush(c);
				} else if (t == XCB_PROPERTY_NOTIFY) {
					xcb_property_notify_event_t *pn = (void *)e;
					if (incr_to && pn->window == incr_to && pn->atom == incr_prop && pn->state == XCB_PROPERTY_DELETE) {
						size_t n = len - incr_at < CHUNK ? len - incr_at : CHUNK;
						xcb_change_property(c, XCB_PROP_MODE_REPLACE, incr_to, incr_prop, incr_type, 8, (uint32_t)n, data + incr_at);
						incr_at += n;
						if (n == 0) incr_to = 0;
						xcb_flush(c);
					}
				} else if (t == XCB_SELECTION_CLEAR) {
					fprintf(stderr, "copy: lost the clipboard\n");
					return 0;
				}
				free(e);
			}
		}
		return 0;
	}
	xcb_atom_t target = !strcmp(argv[1], "targets") ? targets : argc > 2 ? atom(argv[2]) : utf8;
	xcb_convert_selection(c, w, clipboard, target, prop, XCB_CURRENT_TIME);
	xcb_flush(c);
	size_t total = 0;
	bool incr_mode = false;
	time_t end = time(NULL) + 5;
	while (time(NULL) < end) {
		struct pollfd p = {xcb_get_file_descriptor(c), POLLIN, 0};
		poll(&p, 1, 100);
		xcb_generic_event_t *e;
		while ((e = xcb_poll_for_event(c))) {
			uint8_t t = e->response_type & 0x7f;
			bool read_it = false;
			if (t == XCB_SELECTION_NOTIFY) {
				xcb_selection_notify_event_t *n = (void *)e;
				if (!n->property) { fprintf(stderr, "paste: refused\n"); return 1; }
				read_it = true;
			} else if (t == XCB_PROPERTY_NOTIFY && incr_mode) {
				xcb_property_notify_event_t *pn = (void *)e;
				read_it = pn->atom == prop && pn->state == XCB_PROPERTY_NEW_VALUE;
			}
			free(e);
			if (!read_it) continue;
			xcb_get_property_reply_t *r = xcb_get_property_reply(c, xcb_get_property(c, 1, w, prop, XCB_GET_PROPERTY_TYPE_ANY, 0, 0x7fffffff), NULL);
			if (!r) return 1;
			if (r->type == incr) { incr_mode = true; free(r); xcb_flush(c); continue; }
			int n = xcb_get_property_value_length(r);
			if (target == targets) {
				xcb_atom_t *list = xcb_get_property_value(r);
				for (int i = 0; i < n / 4; i++) {
					xcb_get_atom_name_reply_t *an = xcb_get_atom_name_reply(c, xcb_get_atom_name(c, list[i]), NULL);
					if (an) printf("%.*s\n", xcb_get_atom_name_name_length(an), xcb_get_atom_name_name(an));
					free(an);
				}
				return 0;
			}
			fwrite(xcb_get_property_value(r), 1, (size_t)n, stdout);
			total += (size_t)n;
			free(r);
			xcb_flush(c);
			if (!incr_mode || n == 0) { fflush(stdout); fprintf(stderr, "paste: %zu bytes%s\n", total, incr_mode ? " (incr)" : ""); return 0; }
		}
	}
	fprintf(stderr, "paste: timed out after %zu bytes\n", total);
	return 1;
}
