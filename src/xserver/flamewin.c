#include <stdlib.h>
#include <string.h>

#include "xserver/internal.h"

// windows flame itself keeps in the X tree, no client behind them: the
// selection owner, a selection's answer, the drag and drop proxy. clients
// see them like any other window, flame hears the ClientMessages sent to
// them (hooks.client_message)

#define FLAME_WINDOWS_BASE 0x18000 // above the selection transfers' range

struct xs_window *xserver_flame_window_create(struct flame_xserver *xserver) {
	uint32_t id = FLAME_WINDOWS_BASE + (xserver->flame_window_ids++ & 0x7fff);
	while (xserver_window(xserver, id)) {
		id = FLAME_WINDOWS_BASE + (xserver->flame_window_ids++ & 0x7fff);
	}
	struct xs_window *window = xs_window_create(xserver, NULL, id, xserver->root);
	if (window) {
		window->class = 2; // InputOnly
	}
	return window;
}

void xserver_flame_window_destroy(struct flame_xserver *xserver, struct xs_window *window) {
	xs_window_destroy(xserver, window);
}

void xserver_flame_window_show(struct flame_xserver *xserver, struct xs_window *window,
		int x, int y, int width, int height) {
	xs_window_configure(xserver, window, 1 | 2 | 4 | 8 | 64, x, y, width, height, 0, 0,
		0 /* Above: on top of everything */);
	if (!window->mapped) {
		xs_window_map(xserver, window);
	}
}

void xserver_raise_window(struct flame_xserver *xserver, struct xs_window *window) {
	if (window->parent && window->link.next != &window->parent->children) {
		xs_window_configure(xserver, window, 64, 0, 0, 0, 0, 0, 0, 0 /* Above */);
	}
}

void xserver_flame_window_hide(struct flame_xserver *xserver, struct xs_window *window) {
	if (window->mapped) {
		xs_window_unmap(xserver, window);
	}
}

bool xserver_set_property(struct flame_xserver *xserver, struct xs_window *window,
		uint32_t name, uint32_t type, uint8_t format, const void *data, uint32_t units) {
	uint8_t error;
	return xs_window_change_property(xserver, window, 0, name, type, format, units, data,
		&error);
}

void xserver_delete_property(struct flame_xserver *xserver, struct xs_window *window,
		uint32_t name) {
	xs_window_delete_property(xserver, window, name);
}

void xserver_send_client_message(struct flame_xserver *xserver, uint32_t window,
		uint32_t type, const uint32_t data[5]) {
	struct xs_window *to = xserver_window(xserver, window);
	if (!to || !to->owner) {
		return;
	}
	struct xs_event event = {0};
	xs_event_put8(&event, 0, 33 | 0x80); // ClientMessage, from SendEvent
	xs_event_put8(&event, 1, 32);
	xs_event_put32(&event, 4, window);
	xs_event_put32(&event, 8, type);
	for (int i = 0; i < 5; i++) {
		xs_event_put32(&event, 12 + (size_t)i * 4, data[i]);
	}
	xs_event_send(to->owner, &event);
}

void xs_flame_window_event(struct flame_xserver *xserver, struct xs_window *window,
		const struct xs_event *event) {
	uint8_t type = event->bytes[0] & 0x7f;
	if (type != 33) {
		xs_selection_window_event(xserver, window, event);
		return;
	}
	if (xserver->hooks.client_message && event->bytes[1] == 32) {
		uint32_t message, data[5];
		memcpy(&message, event->bytes + 8, 4);
		memcpy(data, event->bytes + 12, 20);
		xserver->hooks.client_message(xserver->hooks.data, window->id, message, data);
	}
}

struct xs_window *xserver_top_level_at(struct flame_xserver *xserver, int x, int y) {
	return xs_window_child_at(xserver->root, x, y);
}

void xserver_window_set_hidden(struct flame_xserver *xserver, struct xs_window *window,
		bool hidden) {
	window->wm_hidden = hidden;
}

void xserver_window_root_position(struct xs_window *window, int *x, int *y) {
	xs_window_origin(window, x, y);
}

bool xserver_window_accepts_input(const struct xs_window *window, int x, int y) {
	return xs_shape_contains(window, x, y);
}
