// a virtual machine's window: it takes flame's shortcuts while it has the
// keyboard (keyboard-shortcuts-inhibit), says when it has them and when it
// doesn't, and how many keys it got
// usage: wlshortcuts SECONDS
#define _GNU_SOURCE
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct xdg_wm_base *wm_base;
static struct zwp_keyboard_shortcuts_inhibit_manager_v1 *manager;
static struct wl_surface *surface;
static bool configured;
static int keys;
static int want_w = 400, want_h = 300;

static struct wl_buffer *make_buffer(int w, int h) {
	int stride = w * 4, size = stride * h;
	int fd = memfd_create("wlshortcuts", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) < 0) {
		exit(1);
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
		WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buffer;
}

static void xs_configure(void *data, struct xdg_surface *xs, uint32_t serial) {
	xdg_surface_ack_configure(xs, serial);
	wl_surface_attach(surface, make_buffer(want_w, want_h), 0, 0);
	wl_surface_commit(surface);
	configured = true;
}

static const struct xdg_surface_listener xs_listener = {xs_configure};

static void top_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h,
		struct wl_array *states) {
	if (w > 0 && h > 0) {
		want_w = w;
		want_h = h;
	}
}

static void top_close(void *data, struct xdg_toplevel *t) {
}

static const struct xdg_toplevel_listener top_listener = {top_configure, top_close};

static void k_keymap(void *data, struct wl_keyboard *k, uint32_t format, int32_t fd,
		uint32_t size) {
	close(fd);
}

static void k_enter(void *data, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s,
		struct wl_array *pressed) {
}

static void k_leave(void *data, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s) {
}

static void k_key(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t time,
		uint32_t key, uint32_t state) {
	keys += state == WL_KEYBOARD_KEY_STATE_PRESSED;
}

static void k_modifiers(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t depressed,
		uint32_t latched, uint32_t locked, uint32_t group) {
}

static const struct wl_keyboard_listener keyboard_listener = {k_keymap, k_enter, k_leave, k_key,
	k_modifiers};

static void inhibitor_active(void *data, struct zwp_keyboard_shortcuts_inhibitor_v1 *i) {
	printf("has the shortcuts, %d keys so far\n", keys);
	fflush(stdout);
}

static void inhibitor_inactive(void *data, struct zwp_keyboard_shortcuts_inhibitor_v1 *i) {
	printf("flame has them back, %d keys so far\n", keys);
	fflush(stdout);
}

static const struct zwp_keyboard_shortcuts_inhibitor_v1_listener inhibitor_listener = {
	inhibitor_active, inhibitor_inactive};

static void wm_ping(void *data, struct xdg_wm_base *b, uint32_t serial) {
	xdg_wm_base_pong(b, serial);
}

static const struct xdg_wm_base_listener wm_listener = {wm_ping};

static void global(void *data, struct wl_registry *r, uint32_t name, const char *iface,
		uint32_t version) {
	if (!strcmp(iface, wl_compositor_interface.name)) {
		compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	} else if (!strcmp(iface, wl_shm_interface.name)) {
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	} else if (!strcmp(iface, wl_seat_interface.name)) {
		seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
	} else if (!strcmp(iface, xdg_wm_base_interface.name)) {
		wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
	} else if (!strcmp(iface, zwp_keyboard_shortcuts_inhibit_manager_v1_interface.name)) {
		manager = wl_registry_bind(r, name,
			&zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1);
	}
}

static void global_remove(void *data, struct wl_registry *r, uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {global, global_remove};

int main(int argc, char **argv) {
	double seconds = argc > 1 ? atof(argv[1]) : 3;
	struct wl_display *display = wl_display_connect(NULL);
	if (!display) {
		return 2;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (!compositor || !shm || !seat || !wm_base || !manager) {
		puts("missing globals");
		return 1;
	}
	xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
	surface = wl_compositor_create_surface(compositor);
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xs, &xs_listener, NULL);
	struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xs);
	xdg_toplevel_add_listener(toplevel, &top_listener, NULL);
	xdg_toplevel_set_title(toplevel, "wlshortcuts");
	wl_surface_commit(surface);
	while (!configured && wl_display_dispatch(display) >= 0) {
	}
	struct wl_keyboard *keyboard = wl_seat_get_keyboard(seat);
	wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
	struct zwp_keyboard_shortcuts_inhibitor_v1 *inhibitor =
		zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(manager, surface, seat);
	zwp_keyboard_shortcuts_inhibitor_v1_add_listener(inhibitor, &inhibitor_listener, NULL);

	struct timespec start, now;
	clock_gettime(CLOCK_MONOTONIC, &start);
	int fd = wl_display_get_fd(display);
	for (;;) {
		wl_display_flush(display);
		clock_gettime(CLOCK_MONOTONIC, &now);
		double left = seconds - (double)(now.tv_sec - start.tv_sec)
			- (double)(now.tv_nsec - start.tv_nsec) / 1e9;
		if (left <= 0) {
			break;
		}
		struct pollfd p = {fd, POLLIN, 0};
		if (poll(&p, 1, (int)(left * 1000) + 1) > 0 && wl_display_dispatch(display) < 0) {
			break;
		}
	}
	printf("%d keys in all\n", keys);
	wl_display_disconnect(display);
	return 0;
}
