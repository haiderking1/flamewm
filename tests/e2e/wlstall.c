// a wayland app that stops reading for a while, busy like a browser can be,
// while flame has a lot to tell it: a frozen browser with a 1000Hz mouse
// moving over it gets a few hundred kB of motion in a few seconds. here it's
// replies to syncs it asks for, the same flood without needing a mouse.
// prints whether flame still talks to it after
// usage: wlstall REPLIES
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wl_surface *surface;
static bool configured;

static struct wl_buffer *make_buffer(int w, int h) {
	int stride = w * 4, size = stride * h;
	int fd = memfd_create("wlstall", MFD_CLOEXEC);
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
	if (!configured) {
		configured = true;
		wl_surface_attach(surface, make_buffer(64, 64), 0, 0);
		wl_surface_commit(surface);
	}
}

static const struct xdg_surface_listener xs_listener = {xs_configure};

static void top_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h,
		struct wl_array *states) {
}

static void top_close(void *data, struct xdg_toplevel *t) {
}

static void top_bounds(void *data, struct xdg_toplevel *t, int32_t w, int32_t h) {
}

static void top_caps(void *data, struct xdg_toplevel *t, struct wl_array *caps) {
}

static const struct xdg_toplevel_listener top_listener = {
	top_configure, top_close, top_bounds, top_caps,
};

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
	} else if (!strcmp(iface, xdg_wm_base_interface.name)) {
		wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
	}
}

static void global_remove(void *data, struct wl_registry *r, uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {global, global_remove};

int main(int argc, char **argv) {
	int replies = argc > 1 ? atoi(argv[1]) : 20000;
	struct wl_display *display = wl_display_connect(NULL);
	if (!display) {
		return 2;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (!compositor || !shm || !wm_base) {
		puts("missing globals");
		return 1;
	}
	xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
	surface = wl_compositor_create_surface(compositor);
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xs, &xs_listener, NULL);
	struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xs);
	xdg_toplevel_add_listener(toplevel, &top_listener, NULL);
	xdg_toplevel_set_title(toplevel, "wlstall");
	wl_surface_commit(surface);
	while (!configured && wl_display_dispatch(display) >= 0) {
	}
	wl_display_roundtrip(display);
	puts("mapped");
	fflush(stdout);

	// busy: asks, never reads. each sync is a done and a delete_id back
	int fd = wl_display_get_fd(display);
	for (int i = 0; i < replies; i++) {
		wl_callback_destroy(wl_display_sync(display));
		while (wl_display_flush(display) < 0) {
			struct pollfd p = {fd, POLLOUT, 0};
			if (errno != EAGAIN || poll(&p, 1, 1000) <= 0) {
				break;
			}
		}
	}
	sleep(1); // flame works through all of them meanwhile

	bool alive = wl_display_roundtrip(display) >= 0;
	printf("after the stall: %s\n", alive ? "still connected" : "dropped");
	wl_display_disconnect(display);
	return 0;
}
