// XKB against flame's X server, read the way kitty, alacritty and qt read it:
// through xkbcommon-x11. run by the harness: make test
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/xkb.h>
#include <xkbcommon/xkbcommon-x11.h>
#include <xkbcommon/xkbcommon.h>

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

static struct xkb_context *context;
static uint8_t xkb_event;

// every keysym on every level of every layout the same, and the same
// modifiers picking each level
static void compare(struct xkb_keymap *local, struct xkb_keymap *remote) {
	CHECK(xkb_keymap_num_layouts(local) == xkb_keymap_num_layouts(remote));
	int keys = 0, differ = 0;
	for (xkb_keycode_t k = 8; k <= 255 && k <= xkb_keymap_max_keycode(local); k++) {
		xkb_layout_index_t layouts = xkb_keymap_num_layouts_for_key(local, k);
		if (xkb_keymap_num_layouts_for_key(remote, k) != layouts) {
			differ++;
			continue;
		}
		for (xkb_layout_index_t g = 0; g < layouts; g++) {
			xkb_level_index_t levels = xkb_keymap_num_levels_for_key(local, k, g);
			for (xkb_level_index_t l = 0; l < levels; l++) {
				const xkb_keysym_t *a, *b;
				int na = xkb_keymap_key_get_syms_by_level(local, k, g, l, &a);
				int nb = xkb_keymap_key_get_syms_by_level(remote, k, g, l, &b);
				if ((na > 0 ? a[0] : 0) != (nb > 0 ? b[0] : 0)) {
					differ++;
				}
				xkb_mod_mask_t ma[8] = {0}, mb[8] = {0};
				size_t ca = xkb_keymap_key_get_mods_for_level(local, k, g, l, ma, 8);
				size_t cb = xkb_keymap_key_get_mods_for_level(remote, k, g, l, mb, 8);
				if (ca != cb || memcmp(ma, mb, ca * sizeof(*ma))) {
					differ++;
				}
			}
		}
		keys++;
	}
	CHECK(keys > 100);
	CHECK(differ == 0);
	if (differ) {
		fprintf(stderr, "compare: %d differences\n", differ);
	}
}

static struct xkb_keymap *remote_keymap(xcb_connection_t *c, int32_t device) {
	struct xkb_keymap *keymap = xkb_x11_keymap_new_from_device(context, c, device,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	CHECK(keymap != NULL);
	return keymap;
}

static struct xkb_keymap *local_keymap(const char *layouts) {
	struct xkb_rule_names names = {.layout = layouts};
	return xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
}

static void drain(xcb_connection_t *c, int *xkb_events, int *mapping_notifies) {
	free(xcb_get_input_focus_reply(c, xcb_get_input_focus(c), NULL));
	xcb_generic_event_t *e;
	while ((e = xcb_poll_for_event(c))) {
		uint8_t type = e->response_type & 0x7f;
		if (type == xkb_event && xkb_events) {
			(*xkb_events)++;
		} else if (type == XCB_MAPPING_NOTIFY && mapping_notifies) {
			(*mapping_notifies)++;
		}
		free(e);
	}
}

static void test_core(xcb_connection_t *c) {
	xcb_get_keyboard_mapping_reply_t *r = xcb_get_keyboard_mapping_reply(c,
		xcb_get_keyboard_mapping(c, 38, 1), NULL); // the a key
	CHECK(r && r->keysyms_per_keycode >= 4);
	if (r) {
		xcb_keysym_t *syms = xcb_get_keyboard_mapping_keysyms(r);
		CHECK(syms[0] == XKB_KEY_a && syms[1] == XKB_KEY_A);
	}
	free(r);
	xcb_get_modifier_mapping_reply_t *m = xcb_get_modifier_mapping_reply(c,
		xcb_get_modifier_mapping(c), NULL);
	CHECK(m != NULL);
	if (m) {
		xcb_keycode_t *codes = xcb_get_modifier_mapping_keycodes(m);
		int per = m->keycodes_per_modifier;
		bool shift = false, control = false, lock = false;
		for (int i = 0; i < per; i++) {
			shift |= codes[0 * per + i] == 50; // Shift_L
			lock |= codes[1 * per + i] == 66; // Caps_Lock
			control |= codes[2 * per + i] == 37; // Control_L
		}
		CHECK(shift && lock && control);
	}
	free(m);
}

int main(void) {
	const char *fds = getenv("HARNESS_FDS");
	if (!fds || sscanf(fds, "%d %d", &to_harness, &from_harness) != 2) {
		fprintf(stderr, "run me with the harness\n");
		return 2;
	}
	context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	xcb_connection_t *c = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(c)) {
		return 2;
	}
	uint16_t major, minor;
	uint8_t base_event, base_error;
	CHECK(xkb_x11_setup_xkb_extension(c, XKB_X11_MIN_MAJOR_XKB_VERSION,
		XKB_X11_MIN_MINOR_XKB_VERSION, XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS, &major, &minor,
		&base_event, &base_error));
	xkb_event = base_event;
	int32_t device = xkb_x11_get_core_keyboard_device_id(c);
	CHECK(device >= 0);

	// the harness starts with the default keymap, same as this one
	struct xkb_keymap *local = local_keymap(NULL);
	struct xkb_keymap *remote = remote_keymap(c, device);
	if (local && remote) {
		compare(local, remote);
	}
	test_core(c);

	// the names on the root, what setxkbmap and layout switchers read
	xcb_intern_atom_reply_t *a = xcb_intern_atom_reply(c, xcb_intern_atom(c, 1, 16,
		"_XKB_RULES_NAMES"), NULL);
	xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
	xcb_get_property_reply_t *names = a ? xcb_get_property_reply(c, xcb_get_property(c, 0,
		screen->root, a->atom, XCB_ATOM_STRING, 0, 256), NULL) : NULL;
	CHECK(names && xcb_get_property_value_length(names) > 10);
	if (names) {
		const char *rules = xcb_get_property_value(names);
		const char *model = rules + strlen(rules) + 1;
		const char *layout = model + strlen(model) + 1;
		CHECK(!strcmp(rules, "evdev") && !strcmp(layout, getenv("XKB_DEFAULT_LAYOUT")
			? getenv("XKB_DEFAULT_LAYOUT") : "us"));
	}
	free(names);
	free(a);

	// the state follows: shift held, caps locked, through StateNotify
	xcb_xkb_select_events(c, XCB_XKB_ID_USE_CORE_KBD, XCB_XKB_EVENT_TYPE_STATE_NOTIFY
		| XCB_XKB_EVENT_TYPE_NEW_KEYBOARD_NOTIFY | XCB_XKB_EVENT_TYPE_MAP_NOTIFY, 0,
		XCB_XKB_EVENT_TYPE_STATE_NOTIFY | XCB_XKB_EVENT_TYPE_NEW_KEYBOARD_NOTIFY
		| XCB_XKB_EVENT_TYPE_MAP_NOTIFY, 0xff, 0xff, NULL);
	struct xkb_state *state = remote ? xkb_x11_state_new_from_device(remote, c, device) : NULL;
	CHECK(state != NULL);
	int xkb_events = 0;
	drain(c, NULL, NULL);
	harness("mods 1 0 2 0");
	drain(c, &xkb_events, NULL);
	CHECK(xkb_events == 1);
	if (state) {
		xkb_state_unref(state);
		state = xkb_x11_state_new_from_device(remote, c, device);
		CHECK(xkb_state_mod_name_is_active(state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_DEPRESSED)
			== 1);
		CHECK(xkb_state_mod_name_is_active(state, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED)
			== 1);
		xkb_state_unref(state);
	}
	harness("mods 0 0 0 0");
	drain(c, NULL, NULL);
	xkb_keymap_unref(remote);
	xkb_keymap_unref(local);

	// two layouts: everyone hears, the new keymap reads back right, and
	// switching layout is a state change
	int mapping_notifies = 0;
	xkb_events = 0;
	harness("layouts us,ara");
	drain(c, &xkb_events, &mapping_notifies);
	CHECK(xkb_events == 2); // NewKeyboardNotify, MapNotify
	CHECK(mapping_notifies == 2);
	local = local_keymap("us,ara");
	remote = remote_keymap(c, device);
	if (local && remote) {
		compare(local, remote);
	}
	harness("mods 0 0 0 1");
	state = remote ? xkb_x11_state_new_from_device(remote, c, device) : NULL;
	CHECK(state && xkb_state_serialize_layout(state, XKB_STATE_LAYOUT_EFFECTIVE) == 1);
	if (state) {
		xkb_state_unref(state);
	}
	// the same keymap again is nothing new
	drain(c, NULL, NULL);
	xkb_events = mapping_notifies = 0;
	harness("layouts us,ara");
	drain(c, &xkb_events, &mapping_notifies);
	CHECK(xkb_events == 0 && mapping_notifies == 0);
	xkb_keymap_unref(remote);
	xkb_keymap_unref(local);

	xcb_disconnect(c);
	xkb_context_unref(context);
	if (failures) {
		fprintf(stderr, "xkb_test: %d failed\n", failures);
		return 1;
	}
	printf("xkb: all good\n");
	return 0;
}
