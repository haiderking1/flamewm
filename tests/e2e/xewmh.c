// flame as X's window manager, the EWMH way an app sees it
// usage: xewmh check | fullscreen | startfull | activate | steal | close | dpi | configure |
//   subpixel | wmstate | warp | confine FLAMECTL | takefocus FLAMECTL | grabaway FLAMECTL
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
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

// where the pointer is in w, and on the root
static void pointer_in(xcb_window_t w, int *x, int *y, int *rx, int *ry) {
	xcb_query_pointer_reply_t *r = xcb_query_pointer_reply(c, xcb_query_pointer(c, w), NULL);
	*x = r ? r->win_x : -1;
	*y = r ? r->win_y : -1;
	*rx = r ? r->root_x : -1;
	*ry = r ? r->root_y : -1;
	free(r);
}

static void listen_motion(xcb_window_t w) {
	uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY
		| XCB_EVENT_MASK_POINTER_MOTION;
	xcb_change_window_attributes(c, w, XCB_CW_EVENT_MASK, &mask);
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
	} else if (!strcmp(mode, "steal")) {
		// another app asking for the focus wine's way (as a pager, no
		// timestamp) while the user is in this one: refused. between its
		// own windows an app moves focus as it likes
		int ready[2], go[2], done[2];
		if (pipe(ready) < 0 || pipe(go) < 0 || pipe(done) < 0) {
			return 1;
		}
		xcb_disconnect(c);
		pid_t child = fork();
		if (child == 0) {
			c = xcb_connect(NULL, NULL);
			s = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
			xcb_window_t b = window(false);
			settle();
			char byte;
			if (write(ready[1], &b, sizeof(b)) != sizeof(b) || read(go[0], &byte, 1) != 1) {
				_exit(1);
			}
			wm_message(b, "_NET_ACTIVE_WINDOW", 2, 0, 0);
			settle();
			// its window stays till the other side looked: gone, the focus
			// would come back anyway
			if (write(ready[1], &b, sizeof(b)) != sizeof(b) || read(done[0], &byte, 1) != 1) {
				_exit(1);
			}
			_exit(0);
		}
		c = xcb_connect(NULL, NULL);
		s = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
		xcb_window_t b;
		if (read(ready[0], &b, sizeof(b)) != sizeof(b)) {
			return 1;
		}
		xcb_window_t a = window(false);
		settle();
		printf("the user is in this app: %s\n",
			first32(s->root, "_NET_ACTIVE_WINDOW") == a ? "yes" : "no");
		if (write(go[1], "x", 1) != 1) {
			return 1;
		}
		if (read(ready[0], &b, sizeof(b)) != sizeof(b)) {
			return 1;
		}
		settle();
		printf("the other app asked, still this one: %s\n",
			first32(s->root, "_NET_ACTIVE_WINDOW") == a ? "yes" : "no");
		if (write(done[1], "x", 1) != 1) {
			return 1;
		}
		waitpid(child, NULL, 0);
		xcb_window_t a2 = window(false);
		settle();
		wm_message(a, "_NET_ACTIVE_WINDOW", 2, 0, 0);
		settle();
		printf("its own window back: %s\n",
			first32(s->root, "_NET_ACTIVE_WINDOW") == a ? "yes" : "no");
		(void)a2;
	} else if (!strcmp(mode, "wmstate")) {
		// ICCCM: Normal once managed, Withdrawn once the app unmaps it
		xcb_window_t w = window(false);
		settle();
		printf("mapped: %u\n", first32(w, "WM_STATE"));
		xcb_unmap_window(c, w);
		settle();
		xcb_get_property_reply_t *r = prop(w, "WM_STATE");
		printf("unmapped: %u, type is WM_STATE: %s\n", first32(w, "WM_STATE"),
			r && r->type == atom("WM_STATE") ? "yes" : "no");
		free(r);
	} else if (!strcmp(mode, "warp")) {
		// an app moves the pointer (wine centering it for mouse look): flame's
		// cursor goes there and the app hears it move
		xcb_window_t w = window(false);
		listen_motion(w);
		settle();
		xcb_warp_pointer(c, XCB_NONE, w, 0, 0, 0, 0, 50, 60);
		settle();
		int x, y, rx, ry;
		pointer_in(w, &x, &y, &rx, &ry);
		printf("pointer in window %d,%d\n", x, y);
		xcb_generic_event_t *e;
		while ((e = xcb_poll_for_event(c))) {
			xcb_motion_notify_event_t *m = (xcb_motion_notify_event_t *)e;
			if ((e->response_type & 0x7f) == XCB_MOTION_NOTIFY && m->event == w) {
				printf("motion %d,%d\n", m->event_x, m->event_y);
			}
			free(e);
		}
		// another app, nothing of its under the pointer, not focused: the
		// pointer isn't its to take
		pid_t pid = fork();
		if (pid == 0) {
			xcb_connection_t *other = xcb_connect(NULL, NULL);
			xcb_warp_pointer(other, XCB_NONE, s->root, 0, 0, 0, 0, 5, 5);
			free(xcb_get_input_focus_reply(other, xcb_get_input_focus(other), NULL));
			xcb_disconnect(other);
			_exit(0);
		}
		waitpid(pid, NULL, 0);
		settle();
		pointer_in(w, &x, &y, &rx, &ry);
		printf("after another app tried: %d,%d\n", x, y);
	} else if (!strcmp(mode, "confine") && argc > 2) {
		// a grab confining the pointer to a box in the window (wine clipping
		// the cursor): it's moved into it, the mouse can't take it out,
		// letting go frees it
		xcb_window_t w = window(false);
		listen_motion(w);
		xcb_window_t box = xcb_generate_id(c);
		xcb_create_window(c, XCB_COPY_FROM_PARENT, box, w, 100, 100, 40, 30, 0,
			XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT, 0, NULL);
		xcb_map_window(c, box);
		settle();
		xcb_grab_pointer_reply_t *g = xcb_grab_pointer_reply(c, xcb_grab_pointer(c, 0, w,
			XCB_EVENT_MASK_POINTER_MOTION, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, box,
			XCB_NONE, XCB_CURRENT_TIME), NULL);
		printf("grabbed: %s\n", g && g->status == XCB_GRAB_STATUS_SUCCESS ? "yes" : "no");
		free(g);
		int x, y, rx, ry;
		pointer_in(w, &x, &y, &rx, &ry);
		printf("moved inside: %s\n", x >= 100 && x < 140 && y >= 100 && y < 130 ? "yes" : "no");
		char cmd[512];
		snprintf(cmd, sizeof(cmd), "%s pointer move 2 3 > /dev/null", argv[2]);
		if (system(cmd) != 0) {
			return 1;
		}
		settle();
		pointer_in(w, &x, &y, &rx, &ry);
		printf("mouse pushed out: %d,%d\n", x, y);
		xcb_ungrab_pointer(c, XCB_CURRENT_TIME);
		settle();
		if (system(cmd) != 0) {
			return 1;
		}
		settle();
		pointer_in(w, &x, &y, &rx, &ry);
		printf("let go: %d,%d\n", rx, ry);
	} else if (!strcmp(mode, "grabaway") && argc > 2) {
		// a game holding the mouse in its window, the user switches workspace:
		// its grab breaks, the mouse is the user's again, others can grab
		xcb_window_t w = window(false);
		listen_motion(w);
		xcb_window_t box = xcb_generate_id(c);
		xcb_create_window(c, XCB_COPY_FROM_PARENT, box, w, 100, 100, 40, 30, 0,
			XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT, 0, NULL);
		xcb_map_window(c, box);
		settle();
		xcb_grab_pointer_reply_t *g = xcb_grab_pointer_reply(c, xcb_grab_pointer(c, 0, w,
			XCB_EVENT_MASK_POINTER_MOTION, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, box,
			XCB_NONE, XCB_CURRENT_TIME), NULL);
		printf("grabbed: %s\n", g && g->status == XCB_GRAB_STATUS_SUCCESS ? "yes" : "no");
		free(g);
		char cmd[600];
		snprintf(cmd, sizeof(cmd), "%s workspace 2 > /dev/null && sleep 0.3 && %s pointer move"
			" 2 3 > /dev/null", argv[2], argv[2]);
		if (system(cmd) != 0) {
			return 1;
		}
		settle();
		int x, y, rx, ry;
		pointer_in(w, &x, &y, &rx, &ry);
		printf("away, the mouse goes: %d,%d\n", rx, ry);
		xcb_connection_t *other = xcb_connect(NULL, NULL);
		g = xcb_grab_pointer_reply(other, xcb_grab_pointer(other, 0, s->root, 0,
			XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, XCB_NONE, XCB_CURRENT_TIME), NULL);
		printf("another app can grab: %s\n",
			g && g->status == XCB_GRAB_STATUS_SUCCESS ? "yes" : "no");
		free(g);
		xcb_disconnect(other);
	} else if (!strcmp(mode, "away") && argc > 2) {
		// on another workspace a window isn't minimized: chromium stops
		// drawing on _NET_WM_STATE_HIDDEN and comes back white
		xcb_window_t w = window(false);
		settle();
		char cmd[300];
		snprintf(cmd, sizeof(cmd), "%s workspace 2 > /dev/null", argv[2]);
		if (system(cmd) != 0) {
			return 1;
		}
		settle();
		printf("away, ");
		print_state(w);
	} else if (!strcmp(mode, "takefocus") && argc > 2) {
		// wine's game window: no input hint, WM_TAKE_FOCUS. WM_STATE comes
		// before the offer (wine drops one that comes first), a click offers
		// again while the app hasn't taken it, not once it has
		xcb_window_t w = xcb_generate_id(c);
		uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
		xcb_create_window(c, 24, w, s->root, 0, 0, 300, 200, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
			s->root_visual, XCB_CW_EVENT_MASK, &mask);
		uint32_t hints[9] = {1, 0}; // InputHint, input false
		xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, XCB_ATOM_WM_HINTS, XCB_ATOM_WM_HINTS,
			32, 9, hints);
		xcb_atom_t take = atom("WM_TAKE_FOCUS"), wm_state = atom("WM_STATE");
		xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, atom("WM_PROTOCOLS"), XCB_ATOM_ATOM,
			32, 1, &take);
		xcb_map_window(c, w);
		xcb_flush(c);
		// till flame takes it on and offers it the keyboard, 3s at most
		int order = 0, state_at = 0, offer_at = 0;
		xcb_generic_event_t *e;
		for (int tries = 0; tries < 300 && !offer_at; tries++) {
			while ((e = xcb_poll_for_event(c))) {
				uint8_t type = e->response_type & 0x7f;
				order++;
				if (type == XCB_PROPERTY_NOTIFY && !state_at
						&& ((xcb_property_notify_event_t *)e)->atom == wm_state) {
					state_at = order;
				} else if (type == XCB_CLIENT_MESSAGE && !offer_at
						&& ((xcb_client_message_event_t *)e)->data.data32[0] == take) {
					offer_at = order;
				}
				free(e);
			}
			nanosleep(&(struct timespec){0, 10000000}, NULL);
		}
		settle();
		while ((e = xcb_poll_for_event(c))) {
			free(e);
		}
		printf("WM_STATE before the offer: %s\n",
			state_at && offer_at && state_at < offer_at ? "yes" : "no");
		xcb_translate_coordinates_reply_t *t = xcb_translate_coordinates_reply(c,
			xcb_translate_coordinates(c, w, s->root, 0, 0), NULL);
		int rx = t ? t->dst_x + 100 : 100;
		int ry = t ? t->dst_y + 100 : 100;
		free(t);
		char cmd[600];
		snprintf(cmd, sizeof(cmd), "%s pointer move %d %d > /dev/null && %s pointer button left"
			" press > /dev/null && %s pointer button left release > /dev/null", argv[2], rx, ry,
			argv[2], argv[2]);
		for (int round = 0; round < 2; round++) {
			if (round == 1) {
				// the app takes it, like wine does
				xcb_set_input_focus(c, XCB_INPUT_FOCUS_PARENT, w, XCB_CURRENT_TIME);
				settle();
				while ((e = xcb_poll_for_event(c))) {
					free(e);
				}
			}
			if (system(cmd) != 0) {
				return 1;
			}
			settle();
			int offers = 0;
			while ((e = xcb_poll_for_event(c))) {
				offers += (e->response_type & 0x7f) == XCB_CLIENT_MESSAGE
					&& ((xcb_client_message_event_t *)e)->data.data32[0] == take;
				free(e);
			}
			printf("%s: %d\n", round == 0 ? "offers on a click, not taken"
				: "offers on a click, taken", offers);
		}
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
	} else if (!strcmp(mode, "configure")) {
		// a tile asking to sit elsewhere: flame keeps it where it is and says
		// so with a synthetic ConfigureNotify, like a window manager
		xcb_window_t w = window(false);
		settle();
		xcb_get_geometry_reply_t *before = xcb_get_geometry_reply(c, xcb_get_geometry(c, w),
			NULL);
		xcb_generic_event_t *e;
		while ((e = xcb_poll_for_event(c))) {
			free(e); // what came before
		}
		uint32_t values[] = {0, 0, 300, 200};
		xcb_configure_window(c, w, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y
			| XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values);
		settle();
		xcb_get_geometry_reply_t *after = xcb_get_geometry_reply(c, xcb_get_geometry(c, w),
			NULL);
		bool told = false;
		while ((e = xcb_poll_for_event(c))) {
			if (e->response_type == (XCB_CONFIGURE_NOTIFY | 0x80)) {
				xcb_configure_notify_event_t *n = (xcb_configure_notify_event_t *)e;
				told |= after && n->x == after->x && n->y == after->y
					&& n->width == after->width;
			}
			free(e);
		}
		printf("kept: %s\n", before && after && before->x == after->x && before->y == after->y
			&& before->width == after->width && after->x != 0 ? "yes" : "no");
		printf("told: %s\n", told ? "yes" : "no");
		free(before);
		free(after);
	} else if (!strcmp(mode, "subpixel")) {
		// a popup over a window from row 200: a pointer at 199.6 is in row 199
		// of the window, at 200.4 in row 200 of the popup. flame and X agree to
		// the pixel, the window never hears of a spot the popup covers
		xcb_window_t w = window(false);
		uint32_t mask = XCB_EVENT_MASK_POINTER_MOTION;
		xcb_change_window_attributes(c, w, XCB_CW_EVENT_MASK, &mask);
		xcb_window_t p = xcb_generate_id(c);
		uint32_t pv[] = {1, XCB_EVENT_MASK_POINTER_MOTION};
		xcb_create_window(c, 24, p, s->root, 50, 200, 100, 100, 0,
			XCB_WINDOW_CLASS_INPUT_OUTPUT, s->root_visual,
			XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, pv);
		xcb_map_window(c, p);
		uint32_t white = s->white_pixel;
		xcb_gcontext_t gc = xcb_generate_id(c);
		xcb_create_gc(c, gc, p, XCB_GC_FOREGROUND, &white);
		xcb_rectangle_t all = {0, 0, 100, 100};
		xcb_poly_fill_rectangle(c, p, gc, 1, &all);
		settle();
		puts("ready");
		fflush(stdout);
		// what the script's pointer moves bring, for a few seconds
		for (int i = 0; i < 40; i++) {
			nanosleep(&(struct timespec){0, 100000000}, NULL);
			xcb_generic_event_t *e;
			while ((e = xcb_poll_for_event(c))) {
				if ((e->response_type & 0x7f) == XCB_MOTION_NOTIFY) {
					xcb_motion_notify_event_t *m = (xcb_motion_notify_event_t *)e;
					printf("%s row %d\n", m->event == p ? "popup" : "window", m->root_y);
					fflush(stdout);
				}
				free(e);
			}
		}
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
