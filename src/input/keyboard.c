#include "input/keyboard.h"

#include <stdlib.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

#include "bindings/keybind.h"
#include "config/config.h"
#include "core/server.h"
#include "util/listener.h"

static bool handle_vt_switch(struct flame_server *server,
		const xkb_keysym_t *syms, int count) {
	for (int i = 0; i < count; i++) {
		if (syms[i] >= XKB_KEY_XF86Switch_VT_1 && syms[i] <= XKB_KEY_XF86Switch_VT_12) {
			unsigned vt = syms[i] - XKB_KEY_XF86Switch_VT_1 + 1;
			return backend_change_vt(&server->backend, vt);
		}
	}
	return false;
}

static bool handle_press(struct flame_keyboard *keyboard, xkb_keycode_t keycode) {
	struct flame_server *server = keyboard->input->server;
	struct xkb_state *state = keyboard->wlr->xkb_state;
	const xkb_keysym_t *syms;

	// ctrl+alt+fN only exists after translation
	int count = xkb_state_key_get_syms(state, keycode, &syms);
	if (handle_vt_switch(server, syms, count)) {
		return true;
	}

	// binds match the unshifted level so mod+shift+1 stays "1", not "!"
	uint32_t mods = wlr_keyboard_get_modifiers(keyboard->wlr);
	xkb_layout_index_t layout = xkb_state_key_get_layout(state, keycode);
	count = xkb_keymap_key_get_syms_by_level(keyboard->wlr->keymap, keycode,
		layout, 0, &syms);
	for (int i = 0; i < count; i++) {
		if (keybind_handle(server, mods, syms[i])) {
			return true;
		}
	}
	return false;
}

static void handle_key(struct wl_listener *listener, void *data) {
	struct flame_keyboard *keyboard = wl_container_of(listener, keyboard, key);
	struct wlr_seat *seat = keyboard->input->seat;
	struct wlr_keyboard_key_event *event = data;

	bool handled = false;
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		// libinput keycodes are evdev, xkb is offset by 8
		handled = handle_press(keyboard, event->keycode + 8);
	}

	// an X11 window has the keyboard: through flame's X server
	if (!handled && xshell_key(&keyboard->input->server->xshell, keyboard->wlr,
			event->keycode, event->state == WL_KEYBOARD_KEY_STATE_PRESSED, event->time_msec)) {
		return;
	}
	if (!handled) {
		wlr_seat_set_keyboard(seat, keyboard->wlr);
		wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode, event->state);
	}
}

static void handle_modifiers(struct wl_listener *listener, void *data) {
	struct flame_keyboard *keyboard = wl_container_of(listener, keyboard, modifiers);
	struct wlr_seat *seat = keyboard->input->seat;

	wlr_seat_set_keyboard(seat, keyboard->wlr);
	wlr_seat_keyboard_notify_modifiers(seat, &keyboard->wlr->modifiers);
	xshell_keyboard(&keyboard->input->server->xshell, keyboard->wlr);
}

static void handle_destroy(struct wl_listener *listener, void *data) {
	struct flame_keyboard *keyboard = wl_container_of(listener, keyboard, destroy);
	struct flame_input *input = keyboard->input;

	listener_disconnect(&keyboard->modifiers);
	listener_disconnect(&keyboard->key);
	listener_disconnect(&keyboard->destroy);
	wl_list_remove(&keyboard->link);
	free(keyboard);

	input_update_capabilities(input);
}

static bool set_default_keymap(struct wlr_keyboard *wlr_keyboard) {
	// empty names means XKB_DEFAULT_LAYOUT and friends from the env
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = context
		? xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS)
		: NULL;

	bool ok = keymap && wlr_keyboard_set_keymap(wlr_keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	return ok;
}

void keyboard_create(struct flame_input *input, struct wlr_input_device *device) {
	const struct flame_config *config = input->server->config;
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

	if (!set_default_keymap(wlr_keyboard)) {
		wlr_log(WLR_ERROR, "failed to load a keymap for %s", device->name);
		return;
	}
	wlr_keyboard_set_repeat_info(wlr_keyboard, config->repeat_rate, config->repeat_delay);

	struct flame_keyboard *keyboard = calloc(1, sizeof(*keyboard));
	if (!keyboard) {
		wlr_log(WLR_ERROR, "out of memory");
		return;
	}
	keyboard->input = input;
	keyboard->wlr = wlr_keyboard;

	listener_connect(&keyboard->modifiers, &wlr_keyboard->events.modifiers,
		handle_modifiers);
	listener_connect(&keyboard->key, &wlr_keyboard->events.key, handle_key);
	listener_connect(&keyboard->destroy, &device->events.destroy, handle_destroy);

	wl_list_insert(&input->keyboards, &keyboard->link);
	wlr_seat_set_keyboard(input->seat, wlr_keyboard);

	// losing the last keyboard drops keyboard focus, so a replugged one has
	// to hand it back or nothing can type until something gets clicked
	input_update_capabilities(input);
	focus_restore_keyboard(&input->server->desktop);
}
