// keys typed in a wayland window, the way X apps see them on Xorg: push to
// talk listening for raw key events (mumble), polling QueryKeymap (obs,
// discord), and a global hotkey grabbed on the root (hotkey daemons)
// usage: xkeys FLAMECTL
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <xcb/xcb.h>
#include <xcb/xinput.h>

static xcb_connection_t *c;
static const char *ctl;
static uint8_t xi_opcode;

static void settle(void) {
	free(xcb_get_input_focus_reply(c, xcb_get_input_focus(c), NULL));
	nanosleep(&(struct timespec){0, 100000000}, NULL); // under foot's repeat delay
}

static void key(const char *name, const char *how) {
	char cmd[512];
	snprintf(cmd, sizeof(cmd), "%s key %s %s > /dev/null", ctl, name, how);
	if (system(cmd) != 0) {
		exit(1);
	}
	settle();
}

static uint8_t keycode_of(uint32_t keysym) {
	const xcb_setup_t *setup = xcb_get_setup(c);
	uint8_t min = setup->min_keycode, max = setup->max_keycode;
	xcb_get_keyboard_mapping_reply_t *map = xcb_get_keyboard_mapping_reply(c,
		xcb_get_keyboard_mapping(c, min, (uint8_t)(max - min + 1)), NULL);
	uint8_t found = 0;
	if (map) {
		xcb_keysym_t *syms = xcb_get_keyboard_mapping_keysyms(map);
		int per = map->keysyms_per_keycode;
		for (int k = 0; k <= max - min && !found; k++) {
			for (int i = 0; i < per; i++) {
				if (syms[k * per + i] == keysym) {
					found = (uint8_t)(min + k);
				}
			}
		}
	}
	free(map);
	return found;
}

static bool held(uint8_t keycode) {
	xcb_query_keymap_reply_t *r = xcb_query_keymap_reply(c, xcb_query_keymap(c), NULL);
	bool down = r && (r->keys[keycode / 8] >> (keycode % 8) & 1);
	free(r);
	return down;
}

// raw key events and core key presses on the root since last time
static void drain(int *raw, int *hotkey, uint8_t hot) {
	*raw = 0;
	*hotkey = 0;
	xcb_generic_event_t *e;
	while ((e = xcb_poll_for_event(c))) {
		uint8_t type = e->response_type & 0x7f;
		if (type == XCB_GE_GENERIC) {
			xcb_ge_generic_event_t *g = (xcb_ge_generic_event_t *)e;
			// from the keyboard itself: the master's copy comes too, like
			// Xorg sends it (mumble skips those)
			xcb_input_raw_key_press_event_t *r = (xcb_input_raw_key_press_event_t *)e;
			if (g->extension == xi_opcode && (g->event_type == XCB_INPUT_RAW_KEY_PRESS
					|| g->event_type == XCB_INPUT_RAW_KEY_RELEASE) && r->deviceid != 3) {
				(*raw)++;
			}
		} else if (type == XCB_KEY_PRESS && ((xcb_key_press_event_t *)e)->detail == hot) {
			(*hotkey)++;
		}
		free(e);
	}
}

int main(int argc, char **argv) {
	if (argc < 2) {
		return 2;
	}
	ctl = argv[1];
	c = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(c)) {
		return 2;
	}
	xcb_screen_t *s = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
	const xcb_query_extension_reply_t *xi = xcb_get_extension_data(c, &xcb_input_id);
	xi_opcode = xi ? xi->major_opcode : 0;
	free(xcb_input_xi_query_version_reply(c, xcb_input_xi_query_version(c, 2, 2), NULL));
	struct {
		xcb_input_event_mask_t head;
		uint32_t mask;
	} raw = {
		{XCB_INPUT_DEVICE_ALL, 1},
		XCB_INPUT_XI_EVENT_MASK_RAW_KEY_PRESS | XCB_INPUT_XI_EVENT_MASK_RAW_KEY_RELEASE,
	};
	xcb_input_xi_select_events(c, s->root, 1, &raw.head);
	uint8_t a = keycode_of('a'), f9 = keycode_of(0xffc6);
	xcb_void_cookie_t grab = xcb_grab_key_checked(c, 0, s->root, 0, f9, XCB_GRAB_MODE_ASYNC,
		XCB_GRAB_MODE_ASYNC);
	xcb_generic_error_t *error = xcb_request_check(c, grab);
	printf("hotkey grabbed: %s\n", error ? "no" : "yes");
	free(error);
	// someone else can't take the same one
	xcb_connection_t *other = xcb_connect(NULL, NULL);
	error = xcb_request_check(other, xcb_grab_key_checked(other, 0, s->root, 0, f9,
		XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC));
	printf("taken twice: %s\n", error && error->error_code == XCB_ACCESS ? "no" : "yes");
	free(error);
	xcb_disconnect(other);
	settle();
	int raws, hotkeys;
	drain(&raws, &hotkeys, f9);

	key("a", "press");
	printf("held while typed in wayland: %s\n", held(a) ? "yes" : "no");
	key("a", "release");
	printf("up after: %s\n", held(a) ? "no" : "yes");
	drain(&raws, &hotkeys, f9);
	printf("raw events: %d\n", raws);

	key("F9", "press");
	key("F9", "release");
	drain(&raws, &hotkeys, f9);
	printf("hotkey presses: %d\n", hotkeys);

	xcb_ungrab_key(c, f9, s->root, 0);
	settle();
	key("F9", "press");
	key("F9", "release");
	drain(&raws, &hotkeys, f9);
	printf("hotkey presses after ungrab: %d\n", hotkeys);
	xcb_disconnect(c);
	return 0;
}
