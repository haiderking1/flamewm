// a see-through window with glass behind it: flame-eyecandy-v1 ("candy",
// a rounded tinted pane) or ext-background-effect-v1 ("blur", the whole
// window). fixed size, so flame floats it over what's below.
// usage: wlglass candy|blur [seconds]
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "ext-background-effect-v1-client-protocol.h"
#include "flame-eyecandy-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define W 400
#define H 300

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm;
static struct flame_eyecandy_manager_v1 *candy;
static struct ext_background_effect_manager_v1 *background;
static int configured;

static void global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version) {
	if (!strcmp(interface, wl_compositor_interface.name)) {
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (!strcmp(interface, wl_shm_interface.name)) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (!strcmp(interface, xdg_wm_base_interface.name)) {
		wm = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
	} else if (!strcmp(interface, flame_eyecandy_manager_v1_interface.name)) {
		candy = wl_registry_bind(registry, name, &flame_eyecandy_manager_v1_interface, 1);
	} else if (!strcmp(interface, ext_background_effect_manager_v1_interface.name)) {
		background = wl_registry_bind(registry, name,
			&ext_background_effect_manager_v1_interface, 1);
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
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel) {
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
};

int main(int argc, char **argv) {
	int use_candy = argc > 1 && !strcmp(argv[1], "candy");
	int seconds = argc > 2 ? atoi(argv[2]) : 4;
	struct wl_display *display = wl_display_connect(NULL);
	if (!display) {
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (!compositor || !shm || !wm || (use_candy ? !candy : !background)) {
		printf("no %s\n", use_candy ? "flame_eyecandy_manager_v1" : "ext_background_effect");
		return 1;
	}
	xdg_wm_base_add_listener(wm, &wm_listener, NULL);
	struct wl_surface *surface = wl_compositor_create_surface(compositor);
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm, surface);
	xdg_surface_add_listener(xs, &surface_listener, NULL);
	struct xdg_toplevel *top = xdg_surface_get_toplevel(xs);
	xdg_toplevel_add_listener(top, &toplevel_listener, NULL);
	xdg_toplevel_set_title(top, "glass");
	xdg_toplevel_set_app_id(top, "wlglass");
	// a fixed size: flame floats it over whatever's there
	xdg_toplevel_set_min_size(top, W, H);
	xdg_toplevel_set_max_size(top, W, H);
	if (use_candy) {
		struct flame_glass_v1 *glass = flame_eyecandy_manager_v1_get_glass(candy, surface);
		flame_glass_v1_add_pane(glass, 0, 0, W, H, wl_fixed_from_int(24), wl_fixed_from_int(20),
			0x00000030, wl_fixed_from_double(1.6), wl_fixed_from_double(0.6),
			wl_fixed_from_int(6));
	} else {
		struct ext_background_effect_surface_v1 *effect =
			ext_background_effect_manager_v1_get_background_effect(background, surface);
		struct wl_region *region = wl_compositor_create_region(compositor);
		wl_region_add(region, 0, 0, W, H);
		ext_background_effect_surface_v1_set_blur_region(effect, region);
		wl_region_destroy(region);
	}
	wl_surface_commit(surface);
	while (!configured && wl_display_dispatch(display) >= 0) {
	}
	// all clear: only the glass shows
	int size = W * H * 4;
	int fd = memfd_create("wlglass", 0);
	if (fd < 0 || ftruncate(fd, size) < 0) {
		return 1;
	}
	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	memset(data, 0, size);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, W, H, W * 4,
		WL_SHM_FORMAT_ARGB8888);
	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, W, H);
	wl_surface_commit(surface);
	wl_display_flush(display);
	printf("glass up\n");
	fflush(stdout);
	struct timespec until;
	clock_gettime(CLOCK_MONOTONIC, &until);
	until.tv_sec += seconds;
	for (;;) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec > until.tv_sec || (now.tv_sec == until.tv_sec
				&& now.tv_nsec >= until.tv_nsec)) {
			break;
		}
		wl_display_dispatch_pending(display);
		wl_display_flush(display);
		usleep(20000);
	}
	return 0;
}
