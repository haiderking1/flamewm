// wayland drag and drop test client
//   wldnd source TEXT   a window; press a button in it and it drags TEXT (with an icon),
//                       a file:// TEXT as a uri list too
//   wldnd target        a window; accepts text drops, prints "dropped: TEXT", exits
//   wldnd fixed         a window that only comes in 320x240
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdbool.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wl_seat *seat;
static struct wl_data_device_manager *ddm;
static struct wl_data_device *device;
static struct wl_pointer *pointer;
static struct wl_surface *surface;
static struct xdg_toplevel *toplevel;
static int width = 300, height = 200;
static bool configured, done;
static const char *mode, *text;
static struct wl_data_offer *current_offer;
static uint32_t color;

static struct wl_buffer *make_buffer(int w, int h, uint32_t argb) {
	int stride = w * 4, size = stride * h;
	int fd = memfd_create("wldnd", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) < 0) {
		exit(1);
	}
	uint32_t *px = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	for (int i = 0; i < w * h; i++) {
		px[i] = argb;
	}
	munmap(px, (size_t)size);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
		WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buffer;
}

static void draw(void) {
	wl_surface_attach(surface, make_buffer(width, height, color), 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, width, height);
	wl_surface_commit(surface);
}

// ---- source ----

static void source_target(void *data, struct wl_data_source *s, const char *mime) {
	printf("source: target accepts %s\n", mime ? mime : "(nothing)");
	fflush(stdout);
}

static void source_send(void *data, struct wl_data_source *s, const char *mime, int32_t fd) {
	printf("source: send %s\n", mime);
	fflush(stdout);
	if (write(fd, text, strlen(text)) < 0) {
		perror("write");
	}
	close(fd);
}

static void source_cancelled(void *data, struct wl_data_source *s) {
	printf("source: cancelled\n");
	fflush(stdout);
	wl_data_source_destroy(s);
}

static void source_drop_performed(void *data, struct wl_data_source *s) {
	printf("source: drop performed\n");
	fflush(stdout);
}

static void source_finished(void *data, struct wl_data_source *s) {
	printf("source: finished\n");
	fflush(stdout);
	wl_data_source_destroy(s);
}

static void source_action(void *data, struct wl_data_source *s, uint32_t action) {
	printf("source: action %u\n", action);
	fflush(stdout);
}

static const struct wl_data_source_listener source_listener = {
	source_target, source_send, source_cancelled, source_drop_performed, source_finished,
	source_action,
};

// ---- pointer ----

static void pointer_enter(void *data, struct wl_pointer *p, uint32_t serial,
		struct wl_surface *s, wl_fixed_t x, wl_fixed_t y) {}
static void pointer_leave(void *data, struct wl_pointer *p, uint32_t serial,
		struct wl_surface *s) {}
static void pointer_motion(void *data, struct wl_pointer *p, uint32_t t, wl_fixed_t x,
		wl_fixed_t y) {}

static void pointer_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time,
		uint32_t button, uint32_t state) {
	if (strcmp(mode, "source") || state != WL_POINTER_BUTTON_STATE_PRESSED) {
		return;
	}
	struct wl_data_source *source = wl_data_device_manager_create_data_source(ddm);
	wl_data_source_add_listener(source, &source_listener, NULL);
	wl_data_source_offer(source, "text/plain;charset=utf-8");
	wl_data_source_offer(source, "text/plain");
	if (!strncmp(text, "file://", 7)) {
		wl_data_source_offer(source, "text/uri-list");
	}
	wl_data_source_set_actions(source, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY
		| WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
	struct wl_surface *icon = wl_compositor_create_surface(compositor);
	wl_surface_attach(icon, make_buffer(32, 32, 0xffff8800), 0, 0);
	wl_surface_damage_buffer(icon, 0, 0, 32, 32);
	wl_surface_commit(icon);
	wl_data_device_start_drag(device, source, surface, icon, serial);
	printf("source: drag started\n");
	fflush(stdout);
}

static void pointer_axis(void *data, struct wl_pointer *p, uint32_t t, uint32_t a,
		wl_fixed_t v) {}

static void pointer_frame(void *data, struct wl_pointer *p) {}
static void pointer_axis_source(void *data, struct wl_pointer *p, uint32_t s) {}
static void pointer_axis_stop(void *data, struct wl_pointer *p, uint32_t t, uint32_t a) {}
static void pointer_axis_discrete(void *data, struct wl_pointer *p, uint32_t a, int32_t d) {}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter, .leave = pointer_leave, .motion = pointer_motion,
	.button = pointer_button, .axis = pointer_axis, .frame = pointer_frame,
	.axis_source = pointer_axis_source, .axis_stop = pointer_axis_stop,
	.axis_discrete = pointer_axis_discrete,
};

// ---- target ----

static void offer_offer(void *data, struct wl_data_offer *o, const char *mime) {}
static void offer_source_actions(void *data, struct wl_data_offer *o, uint32_t a) {}
static void offer_action(void *data, struct wl_data_offer *o, uint32_t a) {
	printf("target: action %u\n", a);
	fflush(stdout);
}

static const struct wl_data_offer_listener offer_listener = {
	offer_offer, offer_source_actions, offer_action,
};

static void device_data_offer(void *data, struct wl_data_device *d, struct wl_data_offer *o) {
	wl_data_offer_add_listener(o, &offer_listener, NULL);
}

static void device_enter(void *data, struct wl_data_device *d, uint32_t serial,
		struct wl_surface *s, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *o) {
	printf("%s: enter %d,%d\n", mode, wl_fixed_to_int(x), wl_fixed_to_int(y));
	fflush(stdout);
	current_offer = o;
	if (!strcmp(mode, "target") && o) {
		wl_data_offer_accept(o, serial, "text/plain;charset=utf-8");
		wl_data_offer_set_actions(o, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY,
			WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
	}
}

static void device_leave(void *data, struct wl_data_device *d) {
	printf("%s: leave\n", mode);
	fflush(stdout);
	current_offer = NULL;
}

static void device_motion(void *data, struct wl_data_device *d, uint32_t t, wl_fixed_t x,
		wl_fixed_t y) {}

static void device_drop(void *data, struct wl_data_device *d) {
	if (strcmp(mode, "target") || !current_offer) {
		return;
	}
	int fds[2];
	if (pipe(fds) < 0) {
		exit(1);
	}
	wl_data_offer_receive(current_offer, "text/plain;charset=utf-8", fds[1]);
	close(fds[1]);
	wl_display_flush(display);
	char buf[4096];
	size_t len = 0;
	ssize_t n;
	struct pollfd p = {fds[0], POLLIN, 0};
	while (len < sizeof(buf) - 1 && poll(&p, 1, 3000) > 0
			&& (n = read(fds[0], buf + len, sizeof(buf) - 1 - len)) > 0) {
		len += (size_t)n;
	}
	buf[len] = 0;
	close(fds[0]);
	printf("dropped: %s\n", buf);
	fflush(stdout);
	wl_data_offer_finish(current_offer);
	wl_display_flush(display);
	done = true;
}

static void device_selection(void *data, struct wl_data_device *d, struct wl_data_offer *o) {}

static const struct wl_data_device_listener device_listener = {
	device_data_offer, device_enter, device_leave, device_motion, device_drop,
	device_selection,
};

// ---- shell ----

static void wm_ping(void *data, struct xdg_wm_base *b, uint32_t serial) {
	xdg_wm_base_pong(b, serial);
}
static const struct xdg_wm_base_listener wm_listener = {wm_ping};

static void xs_configure(void *data, struct xdg_surface *xs, uint32_t serial) {
	xdg_surface_ack_configure(xs, serial);
	configured = true;
	draw();
}
static const struct xdg_surface_listener xs_listener = {xs_configure};

static void top_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h,
		struct wl_array *states) {
	if (w > 0 && h > 0) {
		width = w;
		height = h;
	}
}
static void top_close(void *data, struct xdg_toplevel *t) {
	done = true;
}
static const struct xdg_toplevel_listener top_listener = {top_configure, top_close};

static void global(void *data, struct wl_registry *r, uint32_t name, const char *iface,
		uint32_t version) {
	if (!strcmp(iface, wl_compositor_interface.name)) {
		compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	} else if (!strcmp(iface, wl_shm_interface.name)) {
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	} else if (!strcmp(iface, xdg_wm_base_interface.name)) {
		wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
	} else if (!strcmp(iface, wl_seat_interface.name)) {
		seat = wl_registry_bind(r, name, &wl_seat_interface, 5);
	} else if (!strcmp(iface, wl_data_device_manager_interface.name)) {
		ddm = wl_registry_bind(r, name, &wl_data_device_manager_interface, 3);
	}
}
static void global_remove(void *data, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener registry_listener = {global, global_remove};

int main(int argc, char **argv) {
	mode = argc > 1 ? argv[1] : "target";
	text = argc > 2 ? argv[2] : "";
	color = !strcmp(mode, "source") ? 0xff2060c0 : 0xff20a040;
	display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "no wayland\n");
		return 2;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
	device = wl_data_device_manager_get_data_device(ddm, seat);
	wl_data_device_add_listener(device, &device_listener, NULL);
	pointer = wl_seat_get_pointer(seat);
	wl_pointer_add_listener(pointer, &pointer_listener, NULL);
	surface = wl_compositor_create_surface(compositor);
	struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(xs, &xs_listener, NULL);
	toplevel = xdg_surface_get_toplevel(xs);
	xdg_toplevel_add_listener(toplevel, &top_listener, NULL);
	xdg_toplevel_set_title(toplevel, mode);
	if (!strcmp(mode, "fixed")) {
		xdg_toplevel_set_min_size(toplevel, 320, 240);
		xdg_toplevel_set_max_size(toplevel, 320, 240);
		width = 320;
		height = 240;
	}
	wl_surface_commit(surface);
	time_t end = time(NULL) + (argc > 3 ? atoi(argv[3]) : 15);
	while (!done && time(NULL) < end) {
		wl_display_flush(display);
		struct pollfd p = {wl_display_get_fd(display), POLLIN, 0};
		if (poll(&p, 1, 200) > 0 && wl_display_dispatch(display) < 0) {
			break;
		}
		wl_display_dispatch_pending(display);
	}
	return 0;
}
