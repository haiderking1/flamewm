// a wayland game's mouse look: it locks the pointer and looks around by
// how far the mouse moves (relative-pointer). says when the lock holds and
// when it's let go, then how far it heard the mouse go and whether the
// pointer itself moved while locked
// confine: the pointer kept in a 100x100 box at 50,50 in it instead (a
// strategy game's edge scrolling), says the farthest it saw the pointer go
// usage: wlmouselook SECONDS [confine]
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

#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct xdg_wm_base *wm_base;
static struct zwp_pointer_constraints_v1 *constraints;
static struct zwp_relative_pointer_manager_v1 *relative_manager;
static struct wl_surface *surface;
static bool configured, locked;
static double rel_x, rel_y;
static int moved_while_locked;
static bool confined;
static double max_x = -1, max_y = -1, min_x = 1e9, min_y = 1e9;

static struct wl_buffer *make_buffer(int w, int h) {
	int stride = w * 4, size = stride * h;
	int fd = memfd_create("wlmouselook", MFD_CLOEXEC);
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

static void p_enter(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s,
		wl_fixed_t x, wl_fixed_t y) {
}

static void p_leave(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) {
}

static void p_motion(void *data, struct wl_pointer *p, uint32_t time, wl_fixed_t x,
		wl_fixed_t y) {
	moved_while_locked += locked;
	if (confined) {
		double px = wl_fixed_to_double(x), py = wl_fixed_to_double(y);
		max_x = px > max_x ? px : max_x;
		max_y = py > max_y ? py : max_y;
		min_x = px < min_x ? px : min_x;
		min_y = py < min_y ? py : min_y;
	}
}

static void p_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time,
		uint32_t button, uint32_t state) {
}

static void p_axis(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis,
		wl_fixed_t value) {
}

static const struct wl_pointer_listener pointer_listener = {p_enter, p_leave, p_motion,
	p_button, p_axis};

static void relative_motion(void *data, struct zwp_relative_pointer_v1 *r, uint32_t hi,
		uint32_t lo, wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t udx, wl_fixed_t udy) {
	rel_x += wl_fixed_to_double(udx);
	rel_y += wl_fixed_to_double(udy);
}

static const struct zwp_relative_pointer_v1_listener relative_listener = {relative_motion};

static void lock_locked(void *data, struct zwp_locked_pointer_v1 *l) {
	locked = true;
	puts("locked");
	fflush(stdout);
}

static void lock_unlocked(void *data, struct zwp_locked_pointer_v1 *l) {
	locked = false;
	puts("unlocked");
	fflush(stdout);
}

static const struct zwp_locked_pointer_v1_listener lock_listener = {lock_locked, lock_unlocked};

static void confine_confined(void *data, struct zwp_confined_pointer_v1 *c) {
	confined = true;
	puts("confined");
	fflush(stdout);
}

static void confine_unconfined(void *data, struct zwp_confined_pointer_v1 *c) {
	confined = false;
	puts("unconfined");
	fflush(stdout);
}

static const struct zwp_confined_pointer_v1_listener confine_listener = {confine_confined,
	confine_unconfined};

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
	} else if (!strcmp(iface, zwp_pointer_constraints_v1_interface.name)) {
		constraints = wl_registry_bind(r, name, &zwp_pointer_constraints_v1_interface, 1);
	} else if (!strcmp(iface, zwp_relative_pointer_manager_v1_interface.name)) {
		relative_manager = wl_registry_bind(r, name,
			&zwp_relative_pointer_manager_v1_interface, 1);
	}
}

static void global_remove(void *data, struct wl_registry *r, uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {global, global_remove};

int main(int argc, char **argv) {
	double seconds = argc > 1 ? atof(argv[1]) : 5;
	struct wl_display *display = wl_display_connect(NULL);
	if (!display) {
		return 2;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (!compositor || !shm || !seat || !wm_base || !constraints || !relative_manager) {
		puts("missing globals");
		return 1;
	}
	xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
	surface = wl_compositor_create_surface(compositor);
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xs, &xs_listener, NULL);
	struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xs);
	xdg_toplevel_add_listener(toplevel, &top_listener, NULL);
	xdg_toplevel_set_title(toplevel, "wlmouselook");
	wl_surface_commit(surface);
	while (!configured && wl_display_dispatch(display) >= 0) {
	}
	struct wl_pointer *pointer = wl_seat_get_pointer(seat);
	wl_pointer_add_listener(pointer, &pointer_listener, NULL);
	struct zwp_relative_pointer_v1 *rel =
		zwp_relative_pointer_manager_v1_get_relative_pointer(relative_manager, pointer);
	zwp_relative_pointer_v1_add_listener(rel, &relative_listener, NULL);
	bool confine = argc > 2 && !strcmp(argv[2], "confine");
	if (confine) {
		struct wl_region *box = wl_compositor_create_region(compositor);
		wl_region_add(box, 50, 50, 100, 100);
		struct zwp_confined_pointer_v1 *c = zwp_pointer_constraints_v1_confine_pointer(
			constraints, surface, pointer, box, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
		zwp_confined_pointer_v1_add_listener(c, &confine_listener, NULL);
		wl_region_destroy(box);
	} else {
		struct zwp_locked_pointer_v1 *lock = zwp_pointer_constraints_v1_lock_pointer(
			constraints, surface, pointer, NULL, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
		zwp_locked_pointer_v1_add_listener(lock, &lock_listener, NULL);
	}
	wl_surface_commit(surface);

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
	if (confine) {
		printf("pointer seen from %.0f,%.0f to %.0f,%.0f\n", min_x, min_y, max_x, max_y);
	} else {
		printf("relative %.0f,%.0f, moved while locked %d\n", rel_x, rel_y, moved_while_locked);
	}
	wl_display_disconnect(display);
	return 0;
}
