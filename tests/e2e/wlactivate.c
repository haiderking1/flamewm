// two windows of one app asking for the first to come up (xdg-activation):
// once out of nowhere a second after they're up, then with the click the
// user made in the second (a link clicked: the browser comes up)
// usage: wlactivate SECONDS
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

#include "xdg-activation-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct xdg_wm_base *wm_base;
static struct xdg_activation_v1 *activation;
static uint32_t click_serial;
static bool clicked;

struct win {
	struct wl_surface *surface;
	bool configured;
	int w, h;
};

static struct win first = {.w = 300, .h = 200}, second = {.w = 300, .h = 200};

static struct wl_buffer *make_buffer(int w, int h) {
	int stride = w * 4, size = stride * h;
	int fd = memfd_create("wlactivate", MFD_CLOEXEC);
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
	struct win *win = data;
	xdg_surface_ack_configure(xs, serial);
	wl_surface_attach(win->surface, make_buffer(win->w, win->h), 0, 0);
	wl_surface_commit(win->surface);
	win->configured = true;
}

static const struct xdg_surface_listener xs_listener = {xs_configure};

static void top_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h,
		struct wl_array *states) {
	struct win *win = data;
	if (w > 0 && h > 0) {
		win->w = w;
		win->h = h;
	}
}

static void top_close(void *data, struct xdg_toplevel *t) {
}

static const struct xdg_toplevel_listener top_listener = {top_configure, top_close};

static void p_enter(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s,
		wl_fixed_t x, wl_fixed_t y) {
}

static void p_leave(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) {
}

static void p_motion(void *data, struct wl_pointer *p, uint32_t time, wl_fixed_t x,
		wl_fixed_t y) {
}

static void p_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time,
		uint32_t button, uint32_t state) {
	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		click_serial = serial;
		clicked = true;
	}
}

static void p_axis(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis,
		wl_fixed_t value) {
}

static const struct wl_pointer_listener pointer_listener = {p_enter, p_leave, p_motion,
	p_button, p_axis};

static void token_done(void *data, struct xdg_activation_token_v1 *token, const char *name) {
	xdg_activation_v1_activate(activation, name, first.surface);
	xdg_activation_token_v1_destroy(token);
}

static const struct xdg_activation_token_v1_listener token_listener = {token_done};

// a token for bringing up the first window, with the user's click or not
static void ask(bool with_click) {
	struct xdg_activation_token_v1 *token = xdg_activation_v1_get_activation_token(activation);
	xdg_activation_token_v1_add_listener(token, &token_listener, NULL);
	if (with_click) {
		xdg_activation_token_v1_set_serial(token, click_serial, seat);
		xdg_activation_token_v1_set_surface(token, second.surface);
	}
	xdg_activation_token_v1_commit(token);
}

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
	} else if (!strcmp(iface, xdg_activation_v1_interface.name)) {
		activation = wl_registry_bind(r, name, &xdg_activation_v1_interface, 1);
	}
}

static void global_remove(void *data, struct wl_registry *r, uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {global, global_remove};

static void make(struct win *win, const char *title) {
	win->surface = wl_compositor_create_surface(compositor);
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, win->surface);
	xdg_surface_add_listener(xs, &xs_listener, win);
	struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xs);
	xdg_toplevel_add_listener(toplevel, &top_listener, win);
	xdg_toplevel_set_title(toplevel, title);
	wl_surface_commit(win->surface);
}

static double since(const struct timespec *start) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)(now.tv_sec - start->tv_sec) + (double)(now.tv_nsec - start->tv_nsec) / 1e9;
}

int main(int argc, char **argv) {
	double seconds = argc > 1 ? atof(argv[1]) : 4;
	struct wl_display *display = wl_display_connect(NULL);
	if (!display) {
		return 2;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (!compositor || !shm || !seat || !wm_base || !activation) {
		puts("missing globals");
		return 1;
	}
	xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
	make(&first, "first");
	while (!first.configured && wl_display_dispatch(display) >= 0) {
	}
	wl_display_roundtrip(display);
	usleep(200000);
	make(&second, "second");
	while (!second.configured && wl_display_dispatch(display) >= 0) {
	}
	struct wl_pointer *pointer = wl_seat_get_pointer(seat);
	wl_pointer_add_listener(pointer, &pointer_listener, NULL);

	struct timespec start;
	clock_gettime(CLOCK_MONOTONIC, &start);
	bool asked_alone = false, asked_click = false;
	int fd = wl_display_get_fd(display);
	for (;;) {
		double t = since(&start);
		if (!asked_alone && t > 1) {
			asked_alone = true;
			ask(false);
		}
		if (clicked && !asked_click) {
			asked_click = true;
			ask(true);
		}
		wl_display_flush(display);
		double left = seconds - t;
		if (left <= 0) {
			break;
		}
		struct pollfd p = {fd, POLLIN, 0};
		if (poll(&p, 1, 100) > 0 && wl_display_dispatch(display) < 0) {
			break;
		}
	}
	wl_display_disconnect(display);
	return 0;
}
