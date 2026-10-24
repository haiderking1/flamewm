// an X window that says what keys it gets, a line each as they come:
// "press 0x78 state 0x0", "release 0xffeb state 0x40" (the key's keysym,
// unshifted). runs till it's killed
#include <stdio.h>
#include <stdlib.h>
#include <xcb/xcb.h>

static xcb_keysym_t *syms;
static int per;
static uint8_t min_keycode, max_keycode;

static xcb_keysym_t sym_of(uint8_t keycode) {
	if (!syms || keycode < min_keycode || keycode > max_keycode) {
		return 0;
	}
	return syms[(keycode - min_keycode) * per];
}

int main(void) {
	xcb_connection_t *c = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(c)) {
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	const xcb_setup_t *setup = xcb_get_setup(c);
	xcb_screen_t *s = xcb_setup_roots_iterator(setup).data;
	min_keycode = setup->min_keycode;
	max_keycode = setup->max_keycode;
	xcb_get_keyboard_mapping_reply_t *map = xcb_get_keyboard_mapping_reply(c,
		xcb_get_keyboard_mapping(c, min_keycode, (uint8_t)(max_keycode - min_keycode + 1)), NULL);
	if (map) {
		syms = xcb_get_keyboard_mapping_keysyms(map);
		per = map->keysyms_per_keycode;
	}
	xcb_window_t w = xcb_generate_id(c);
	uint32_t values[] = {s->white_pixel, XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE};
	xcb_create_window(c, XCB_COPY_FROM_PARENT, w, s->root, 0, 0, 400, 300, 0,
		XCB_WINDOW_CLASS_INPUT_OUTPUT, s->root_visual, XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK,
		values);
	xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, 7,
		"xtyped");
	xcb_map_window(c, w);
	xcb_flush(c);
	xcb_generic_event_t *e;
	while ((e = xcb_wait_for_event(c))) {
		uint8_t type = e->response_type & 0x7f;
		if (type == XCB_KEY_PRESS || type == XCB_KEY_RELEASE) {
			xcb_key_press_event_t *k = (xcb_key_press_event_t *)e;
			printf("%s 0x%x state 0x%x\n", type == XCB_KEY_PRESS ? "press" : "release",
				sym_of(k->detail), k->state);
		}
		free(e);
	}
	return 0;
}
