// what swayidle does: asks to hear when nobody's touched anything for
// 400ms (ext-idle-notify), says idled and resumed as they come, then how
// many times it went idle. inhibit: shows a window that keeps the screen on
// first (idle-inhibit, a video playing)
// usage: wlidle SECONDS [inhibit]
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

#include "ext-idle-notify-v1-client-protocol.h"
#include "idle-inhibit-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct xdg_wm_base *wm_base;
static struct ext_idle_notifier_v1 *notifier;
static struct zwp_idle_inhibit_manager_v1 *inhibit_manager;
static struct wl_surface *surface;
static bool configured;
static int idled;

static struct wl_buffer *make_buffer(int w, int h) {
	int stride = w * 4, size = stride * h;
	int fd = memfd_create("wlidle", MFD_CLOEXEC);
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

static int want_w = 400, want_h = 300;

static void xs_configure(void *data, struct xdg_surface *xs, uint32_t serial) {
	xdg_surface_ack_configure(xs, serial);
	wl_surface_attach(surface, make_buffer(want_w, want_h), 0, 0);
	wl_surface_damage(surface, 0, 0, want_w, want_h);
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

static void on_idled(void *data, struct ext_idle_notification_v1 *n) {
	idled++;
	puts("idled");
	fflush(stdout);
}

static void on_resumed(void *data, struct ext_idle_notification_v1 *n) {
	puts("resumed");
	fflush(stdout);
}

static const struct ext_idle_notification_v1_listener idle_listener = {on_idled, on_resumed};

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
	} else if (!strcmp(iface, ext_idle_notifier_v1_interface.name)) {
		notifier = wl_registry_bind(r, name, &ext_idle_notifier_v1_interface, 1);
	} else if (!strcmp(iface, zwp_idle_inhibit_manager_v1_interface.name)) {
		inhibit_manager = wl_registry_bind(r, name, &zwp_idle_inhibit_manager_v1_interface, 1);
	}
}

static void global_remove(void *data, struct wl_registry *r, uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {global, global_remove};

int main(int argc, char **argv) {
	double seconds = argc > 1 ? atof(argv[1]) : 3;
	bool inhibit = argc > 2 && !strcmp(argv[2], "inhibit");
	struct wl_display *display = wl_display_connect(NULL);
	if (!display) {
		return 2;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (!compositor || !shm || !seat || !wm_base || !notifier || !inhibit_manager) {
		puts("missing globals");
		return 1;
	}
	if (inhibit) {
		xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
		surface = wl_compositor_create_surface(compositor);
		struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surface);
		xdg_surface_add_listener(xs, &xs_listener, NULL);
		struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xs);
		xdg_toplevel_add_listener(toplevel, &top_listener, NULL);
		xdg_toplevel_set_title(toplevel, "wlidle");
		zwp_idle_inhibit_manager_v1_create_inhibitor(inhibit_manager, surface);
		wl_surface_commit(surface);
		while (!configured && wl_display_dispatch(display) >= 0) {
		}
		wl_display_roundtrip(display);
	}
	struct ext_idle_notification_v1 *n =
		ext_idle_notifier_v1_get_idle_notification(notifier, 400, seat);
	ext_idle_notification_v1_add_listener(n, &idle_listener, NULL);

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
	printf("idled %d times\n", idled);
	wl_display_disconnect(display);
	return 0;
}
