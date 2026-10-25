// a window that says when flame tells it it's out of sight: "suspended" or
// "shown" on every change of the xdg_toplevel suspended state, for
// seconds (default 4). draws one plain buffer, nothing else
// usage: wlsuspend [seconds]
#define _POSIX_C_SOURCE 200809L
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"

#define W 200
#define H 150

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm;
static int configured;
static int suspended = -1; // not told yet

static void global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version) {
	if (!strcmp(interface, wl_compositor_interface.name)) {
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (!strcmp(interface, wl_shm_interface.name)) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (!strcmp(interface, xdg_wm_base_interface.name) && version >= 6) {
		wm = wl_registry_bind(registry, name, &xdg_wm_base_interface, 6);
	}
}

static void global_remove(void *data, struct wl_registry *registry, uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {global, global_remove};

static void ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
	xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_listener = {ping};

static void surface_configure(void *data, struct xdg_surface *surface, uint32_t serial) {
	xdg_surface_ack_configure(surface, serial);
	configured = 1;
}

static const struct xdg_surface_listener surface_listener = {surface_configure};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t w, int32_t h,
		struct wl_array *states) {
	int now = 0;
	uint32_t *state;
	wl_array_for_each(state, states) {
		now = now || *state == XDG_TOPLEVEL_STATE_SUSPENDED;
	}
	if (now != suspended) {
		suspended = now;
		printf("%s\n", now ? "suspended" : "shown");
		fflush(stdout);
	}
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel) {
}

static void toplevel_bounds(void *data, struct xdg_toplevel *toplevel, int32_t w, int32_t h) {
}

static void toplevel_caps(void *data, struct xdg_toplevel *toplevel, struct wl_array *caps) {
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
	.configure_bounds = toplevel_bounds,
	.wm_capabilities = toplevel_caps,
};

int main(int argc, char **argv) {
	int seconds = argc > 1 ? atoi(argv[1]) : 4;
	struct wl_display *display = wl_display_connect(NULL);
	if (!display) {
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (!compositor || !shm || !wm) {
		printf("no xdg_wm_base v6\n");
		return 1;
	}
	xdg_wm_base_add_listener(wm, &wm_listener, NULL);
	struct wl_surface *surface = wl_compositor_create_surface(compositor);
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm, surface);
	xdg_surface_add_listener(xs, &surface_listener, NULL);
	struct xdg_toplevel *top = xdg_surface_get_toplevel(xs);
	xdg_toplevel_add_listener(top, &toplevel_listener, NULL);
	xdg_toplevel_set_app_id(top, "wlsuspend");
	wl_surface_commit(surface);
	while (!configured && wl_display_dispatch(display) >= 0) {
	}
	int size = W * H * 4;
	int fd = memfd_create("wlsuspend", 0);
	if (fd < 0 || ftruncate(fd, size) < 0) {
		return 1;
	}
	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	memset(data, 0x80, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, W, H, W * 4,
		WL_SHM_FORMAT_XRGB8888);
	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, W, H);
	wl_surface_commit(surface);

	struct timespec start, now;
	clock_gettime(CLOCK_MONOTONIC, &start);
	struct pollfd pfd = {wl_display_get_fd(display), POLLIN, 0};
	for (;;) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec - start.tv_sec >= seconds) {
			break;
		}
		wl_display_flush(display);
		if (poll(&pfd, 1, 100) > 0 && wl_display_dispatch(display) < 0) {
			break;
		}
	}
	return 0;
}
