// flame as X's window manager, the EWMH way an app sees it
// usage: xewmh check | fullscreen | startfull | activate | close | dpi
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xcb/xcb.h>

static xcb_connection_t *c;
static xcb_screen_t *s;

static xcb_atom_t atom(const char *name) {
	xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(c,
		xcb_intern_atom(c, 0, (uint16_t)strlen(name), name), NULL);
	xcb_atom_t a = r ? r->atom : XCB_NONE;
	free(r);
	return a;
}

static xcb_get_property_reply_t *prop(xcb_window_t w, const char *name) {
	return xcb_get_property_reply(c, xcb_get_property(c, 0, w, atom(name),
		XCB_GET_PROPERTY_TYPE_ANY, 0, 1024), NULL);
}

static uint32_t first32(xcb_window_t w, const char *name) {
	xcb_get_property_reply_t *r = prop(w, name);
	uint32_t v = r && xcb_get_property_value_length(r) >= 4
		? *(uint32_t *)xcb_get_property_value(r) : 0;
	free(r);
	return v;
}

// the window's _NET_WM_STATE, short names: FULLSCREEN FOCUSED ...
static void print_state(xcb_window_t w) {
	xcb_get_property_reply_t *r = prop(w, "_NET_WM_STATE");
	printf("state:");
	int n = r ? xcb_get_property_value_length(r) / 4 : 0;
	xcb_atom_t *atoms = r ? xcb_get_property_value(r) : NULL;
	for (int i = 0; i < n; i++) {
		xcb_get_atom_name_reply_t *an = xcb_get_atom_name_reply(c,
			xcb_get_atom_name(c, atoms[i]), NULL);
		if (an) {
			int len = xcb_get_atom_name_name_length(an);
			const char *name = xcb_get_atom_name_name(an);
			const char *prefix = "_NET_WM_STATE_";
			size_t skip = len > 14 && !strncmp(name, prefix, 14) ? 14 : 0;
			printf(" %.*s", (int)(len - (int)skip), name + skip);
		}
		free(an);
	}
	printf("\n");
	free(r);
}

static void print_geometry(xcb_window_t w) {
	xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(c, xcb_get_geometry(c, w), NULL);
	xcb_translate_coordinates_reply_t *t = xcb_translate_coordinates_reply(c,
		xcb_translate_coordinates(c, w, s->root, 0, 0), NULL);
	if (g && t) {
		printf("geometry %d,%d %dx%d\n", t->dst_x, t->dst_y, g->width, g->height);
	}
	free(g);
	free(t);
}

static xcb_window_t window(bool fullscreen) {
	xcb_window_t w = xcb_generate_id(c);
	uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
	xcb_create_window(c, 24, w, s->root, 0, 0, 300, 200, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
		s->root_visual, XCB_CW_EVENT_MASK, &mask);
	xcb_atom_t protocols = atom("WM_PROTOCOLS"), del = atom("WM_DELETE_WINDOW");
	xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, protocols, XCB_ATOM_ATOM, 32, 1, &del);
	if (fullscreen) {
		xcb_atom_t full = atom("_NET_WM_STATE_FULLSCREEN");
		xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, atom("_NET_WM_STATE"), XCB_ATOM_ATOM,
			32, 1, &full);
	}
	xcb_map_window(c, w);
	xcb_flush(c);
	return w;
}

// to the window manager, the way EWMH says
static void wm_message(xcb_window_t w, const char *type, uint32_t d0, uint32_t d1,
		uint32_t d2) {
	xcb_client_message_event_t ev = {
		.response_type = XCB_CLIENT_MESSAGE,
		.format = 32,
		.window = w,
		.type = atom(type),
		.data.data32 = {d0, d1, d2, 1, 0},
	};
	xcb_send_event(c, 0, s->root,
		XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
		(const char *)&ev);
	xcb_flush(c);
}

static void settle(void) {
	nanosleep(&(struct timespec){0, 400000000}, NULL);
	free(xcb_get_input_focus_reply(c, xcb_get_input_focus(c), NULL));
}

int main(int argc, char **argv) {
	if (argc < 2) {
		return 2;
	}
	c = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(c)) {
		return 2;
	}
	s = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
	const char *mode = argv[1];
	if (!strcmp(mode, "check")) {
		uint32_t check = first32(s->root, "_NET_SUPPORTING_WM_CHECK");
		xcb_get_property_reply_t *name = check ? prop(check, "_NET_WM_NAME") : NULL;
		printf("wm %.*s, check points at itself: %s\n",
			name ? xcb_get_property_value_length(name) : 0,
			name ? (char *)xcb_get_property_value(name) : "",
			check && first32(check, "_NET_SUPPORTING_WM_CHECK") == check ? "yes" : "no");
		free(name);
		xcb_get_selection_owner_reply_t *cm = xcb_get_selection_owner_reply(c,
			xcb_get_selection_owner(c, atom("_NET_WM_CM_S0")), NULL);
		printf("compositor: %s\n", cm && cm->owner ? "yes" : "no");
		free(cm);
		xcb_get_property_reply_t *sup = prop(s->root, "_NET_SUPPORTED");
		xcb_atom_t full = atom("_NET_WM_STATE_FULLSCREEN");
		int n = sup ? xcb_get_property_value_length(sup) / 4 : 0;
		bool found = false;
		for (int i = 0; i < n; i++) {
			found |= ((xcb_atom_t *)xcb_get_property_value(sup))[i] == full;
		}
		printf("supports fullscreen: %s\n", found ? "yes" : "no");
		free(sup);
	} else if (!strcmp(mode, "fullscreen")) {
		xcb_window_t w = window(false);
		settle();
		print_state(w);
		wm_message(w, "_NET_WM_STATE", 1, atom("_NET_WM_STATE_FULLSCREEN"), 0);
		settle();
		print_state(w);
		print_geometry(w);
		wm_message(w, "_NET_WM_STATE", 2, atom("_NET_WM_STATE_FULLSCREEN"), 0);
		settle();
		print_state(w);
	} else if (!strcmp(mode, "startfull")) {
		xcb_window_t w = window(true);
		settle();
		print_state(w);
		print_geometry(w);
	} else if (!strcmp(mode, "activate")) {
		xcb_window_t a = window(false);
		settle();
		xcb_window_t b = window(false);
		settle();
		printf("active is the new one: %s\n",
			first32(s->root, "_NET_ACTIVE_WINDOW") == b ? "yes" : "no");
		wm_message(a, "_NET_ACTIVE_WINDOW", 1, 0, 0);
		settle();
		printf("active is the first one: %s\n",
			first32(s->root, "_NET_ACTIVE_WINDOW") == a ? "yes" : "no");
		xcb_get_property_reply_t *list = prop(s->root, "_NET_CLIENT_LIST");
		printf("clients: %d\n", list ? xcb_get_property_value_length(list) / 4 : -1);
		free(list);
	} else if (!strcmp(mode, "dpi")) {
		xcb_get_property_reply_t *rm = prop(s->root, "RESOURCE_MANAGER");
		printf("resources: %.*s", rm ? xcb_get_property_value_length(rm) : 0,
			rm ? (char *)xcb_get_property_value(rm) : "");
		free(rm);
		xcb_get_selection_owner_reply_t *xs = xcb_get_selection_owner_reply(c,
			xcb_get_selection_owner(c, atom("_XSETTINGS_S0")), NULL);
		printf("\nxsettings: %s\nscreen %dx%d\n", xs && xs->owner ? "yes" : "no",
			s->width_in_pixels, s->height_in_pixels);
		free(xs);
	} else if (!strcmp(mode, "close")) {
		xcb_window_t w = window(false);
		settle();
		wm_message(w, "_NET_CLOSE_WINDOW", 0, 1, 0);
		settle();
		xcb_atom_t del = atom("WM_DELETE_WINDOW");
		xcb_generic_event_t *e;
		bool asked = false;
		while ((e = xcb_poll_for_event(c))) {
			if ((e->response_type & 0x7f) == XCB_CLIENT_MESSAGE) {
				asked |= ((xcb_client_message_event_t *)e)->data.data32[0] == del;
			}
			free(e);
		}
		printf("asked to close: %s\n", asked ? "yes" : "no");
	}
	xcb_disconnect(c);
	return 0;
}
