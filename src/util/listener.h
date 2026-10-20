#ifndef FLAME_UTIL_LISTENER_H
#define FLAME_UTIL_LISTENER_H

#include <wayland-server-core.h>

static inline void listener_connect(struct wl_listener *listener,
		struct wl_signal *signal, wl_notify_func_t notify) {
	listener->notify = notify;
	wl_signal_add(signal, listener);
}

// wlroots asserts every listener is gone before an object dies, so every
// connect needs one of these on the matching teardown path
static inline void listener_disconnect(struct wl_listener *listener) {
	wl_list_remove(&listener->link);
	wl_list_init(&listener->link);
}

#endif
