// a wayland app that maps any size, turns one size only a while after it's
// up (zen's welcome screen turns 1200x720), then any size again. it draws
// the size it's told, else the size it is
// usage: wlfixed (1s: 600x400 only, 2.5s: any size again, gone at 4s)
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

#include "xdg-shell-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wl_surface *surface;
static int want_w, want_h, cur_w = 300, cur_h = 200;

static struct wl_buffer *make_buffer(int w, int h) {
	int stride = w * 4, size = stride * h;
	int fd = memfd_create("wlfixed", MFD_CLOEXEC);
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

static void draw(void) {
	wl_surface_attach(surface, make_buffer(cur_w, cur_h), 0, 0);
	wl_surface_damage(surface, 0, 0, cur_w, cur_h);
	wl_surface_commit(surface);
}

static void xs_configure(void *data, struct xdg_surface *xs, uint32_t serial) {
	xdg_surface_ack_configure(xs, serial);
	if (want_w > 0 && want_h > 0) {
		cur_w = want_w;
		cur_h = want_h;
	}
	draw();
}

static const struct xdg_surface_listener xs_listener = {xs_configure};

static void top_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h,
		struct wl_array *states) {
	want_w = w;
	want_h = h;
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

// events for a while, seconds
static void run(struct wl_display *display, double seconds) {
	struct timespec start, now;
	clock_gettime(CLOCK_MONOTONIC, &start);
	int fd = wl_display_get_fd(display);
	for (;;) {
		wl_display_flush(display);
		clock_gettime(CLOCK_MONOTONIC, &now);
		double left = seconds - (double)(now.tv_sec - start.tv_sec)
			- (double)(now.tv_nsec - start.tv_nsec) / 1e9;
		if (left <= 0) {
			return;
		}
		struct pollfd p = {fd, POLLIN, 0};
		if (poll(&p, 1, (int)(left * 1000) + 1) > 0 && wl_display_dispatch(display) < 0) {
			exit(1);
		}
	}
}

int main(void) {
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
	xdg_toplevel_set_title(toplevel, "wlfixed");
	wl_surface_commit(surface);
	run(display, 1);
	// one size only now
	xdg_toplevel_set_min_size(toplevel, 600, 400);
	xdg_toplevel_set_max_size(toplevel, 600, 400);
	cur_w = 600;
	cur_h = 400;
	draw();
	run(display, 1.5);
	// any size again
	xdg_toplevel_set_min_size(toplevel, 0, 0);
	xdg_toplevel_set_max_size(toplevel, 0, 0);
	wl_surface_commit(surface);
	run(display, 1.5);
	wl_display_disconnect(display);
	return 0;
}
