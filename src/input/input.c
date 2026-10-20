#include "input/input.h"

#include <wlr/backend.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

#include "core/server.h"
#include "input/keyboard.h"
#include "util/listener.h"

static void handle_new_input(struct wl_listener *listener, void *data) {
	struct flame_input *input = wl_container_of(listener, input, new_input);
	struct wlr_input_device *device = data;

	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		keyboard_create(input, device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		cursor_attach_device(&input->cursor, device);
		break;
	default:
		wlr_log(WLR_DEBUG, "ignoring input device %s", device->name);
		break;
	}
	input_update_capabilities(input);
}

static void handle_request_set_selection(struct wl_listener *listener, void *data) {
	struct flame_input *input = wl_container_of(listener, input, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(input->seat, event->source, event->serial);
}

static void handle_request_set_primary_selection(struct wl_listener *listener, void *data) {
	struct flame_input *input =
		wl_container_of(listener, input, request_set_primary_selection);
	struct wlr_seat_request_set_primary_selection_event *event = data;
	wlr_seat_set_primary_selection(input->seat, event->source, event->serial);
}

void input_keyboard_enter(struct flame_input *input, struct wlr_surface *surface) {
	if (!surface) {
		wlr_seat_keyboard_notify_clear_focus(input->seat);
		return;
	}
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(input->seat);
	if (keyboard) {
		wlr_seat_keyboard_notify_enter(input->seat, surface, keyboard->keycodes,
			keyboard->num_keycodes, &keyboard->modifiers);
	} else {
		wlr_seat_keyboard_notify_enter(input->seat, surface, NULL, 0, NULL);
	}
}

void input_update_capabilities(struct flame_input *input) {
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&input->keyboards)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(input->seat, caps);
}

bool input_init(struct flame_input *input, struct flame_server *server) {
	input->server = server;
	wl_list_init(&input->keyboards);

	input->seat = wlr_seat_create(server->display, "seat0");
	if (!input->seat) {
		wlr_log(WLR_ERROR, "failed to create seat");
		return false;
	}
	if (!cursor_init(&input->cursor, server)) {
		return false;
	}
	drag_init(&input->drag, server);
	dnd_init(&input->dnd, server);

	listener_connect(&input->new_input, &server->backend.wlr->events.new_input,
		handle_new_input);
	// there's always a pointer, with or without a mouse plugged in yet
	input_update_capabilities(input);
	listener_connect(&input->request_set_selection,
		&input->seat->events.request_set_selection, handle_request_set_selection);
	listener_connect(&input->request_set_primary_selection,
		&input->seat->events.request_set_primary_selection,
		handle_request_set_primary_selection);
	return true;
}

void input_finish(struct flame_input *input) {
	listener_disconnect(&input->new_input);
	listener_disconnect(&input->request_set_selection);
	listener_disconnect(&input->request_set_primary_selection);
	dnd_finish(&input->dnd);
	cursor_finish(&input->cursor);
}
