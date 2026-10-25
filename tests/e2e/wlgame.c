// a fullscreen wayland game: an opaque buffer every frame callback for a few
// seconds. vsyncoff: it asks to tear (wp_tearing_control_v1 async), like a
// game with vsync off. busy: after 60 frames it's stuck once for 1.2s before
// drawing the next, like a browser can be. prints how many frames went out
// usage: wlgame SECONDS [vsyncoff|busy]
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

#include "tearing-control-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wp_tearing_control_manager_v1 *tearing;
static struct wl_surface *surface;
static int width, height, frames;
static bool configured;
static struct wl_buffer *buffers[2];

static struct wl_buffer *make_buffer(int w, int h, uint32_t xrgb) {
	int stride = w * 4, size = stride * h;
	int fd = memfd_create("wlgame", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) < 0) {
		exit(1);
	}
	uint32_t *px = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	for (int i = 0; i < w * h; i++) {
		px[i] = xrgb;
	}
	munmap(px, (size_t)size);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
		WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buffer;
}

static void draw(void);
static bool busy;

static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
	wl_callback_destroy(cb);
	if (busy && frames == 60) {
		usleep(1200000);
	}
	draw();
}

static const struct wl_callback_listener frame_listener = {frame_done};

static void draw(void) {
	struct wl_callback *cb = wl_surface_frame(surface);
	wl_callback_add_listener(cb, &frame_listener, NULL);
	wl_surface_attach(surface, buffers[frames % 2], 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, width, height);
	wl_surface_commit(surface);
	frames++;
}

static void xs_configure(void *data, struct xdg_surface *xs, uint32_t serial) {
	xdg_surface_ack_configure(xs, serial);
	if (!configured && width > 0 && height > 0) {
		configured = true;
		buffers[0] = make_buffer(width, height, 0x0000ff00);
		buffers[1] = make_buffer(width, height, 0x0000ee00);
		draw();
	}
}

static const struct xdg_surface_listener xs_listener = {xs_configure};

static void top_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h,
		struct wl_array *states) {
	if (w > 0 && h > 0 && !configured) {
		width = w;
		height = h;
	}
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
	} else if (!strcmp(iface, wp_tearing_control_manager_v1_interface.name)) {
		tearing = wl_registry_bind(r, name, &wp_tearing_control_manager_v1_interface, 1);
	}
}

static void global_remove(void *data, struct wl_registry *r, uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {global, global_remove};

static double now(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
	double seconds = argc > 1 ? atof(argv[1]) : 2;
	bool vsyncoff = argc > 2 && !strcmp(argv[2], "vsyncoff");
	busy = argc > 2 && !strcmp(argv[2], "busy");
	struct wl_display *display = wl_display_connect(NULL);
	if (!display) {
		return 2;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (!compositor || !shm || !wm_base || (vsyncoff && !tearing)) {
		puts("missing globals");
		return 1;
	}
	xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
	surface = wl_compositor_create_surface(compositor);
	if (vsyncoff) {
		struct wp_tearing_control_v1 *control =
			wp_tearing_control_manager_v1_get_tearing_control(tearing, surface);
		wp_tearing_control_v1_set_presentation_hint(control,
			WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC);
	}
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xs, &xs_listener, NULL);
	struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xs);
	xdg_toplevel_add_listener(toplevel, &top_listener, NULL);
	xdg_toplevel_set_title(toplevel, "wlgame");
	xdg_toplevel_set_fullscreen(toplevel, NULL);
	wl_surface_commit(surface);
	double end = now() + seconds;
	while (now() < end) {
		wl_display_flush(display);
		struct pollfd p = {wl_display_get_fd(display), POLLIN, 0};
		if (poll(&p, 1, 50) > 0 && wl_display_dispatch(display) < 0) {
			break;
		}
		wl_display_dispatch_pending(display);
	}
	printf("frames %d in %.1fs\n", frames, seconds);
	wl_display_disconnect(display);
	return 0;
}
