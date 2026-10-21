// sharing one window, the way the screen sharing portal does it: the
// window on ext-foreign-toplevel-list, a capture source for it, a frame
// copied into shared memory. the window is an X one, red, and on a workspace
// that isn't showing: it comes out red anyway
// usage: wlcapture FLAMECTL [TITLE]  (TITLE: a window that's there, else its own)
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xcb/xcb.h>

#include "ext-foreign-toplevel-list-v1-client-protocol.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"

static struct wl_shm *shm;
static struct ext_foreign_toplevel_list_v1 *list;
static struct ext_foreign_toplevel_image_capture_source_manager_v1 *sources;
static struct ext_image_copy_capture_manager_v1 *copier;

static struct ext_foreign_toplevel_handle_v1 *wanted;
static char title[64];
static const char *want_title = "capme";
static int width, height, ready = -1;
static uint32_t format = 0xffffffff;
static bool done, stopped;
static xcb_connection_t *x;

static void handle_title(void *data, struct ext_foreign_toplevel_handle_v1 *handle,
		const char *t) {
	if (!strcmp(t, want_title)) {
		wanted = handle;
	}
	snprintf(title, sizeof(title), "%s", t);
}

static void ignore_str(void *data, struct ext_foreign_toplevel_handle_v1 *handle,
		const char *s) {
}

static void ignore_handle(void *data, struct ext_foreign_toplevel_handle_v1 *handle) {
}

static const struct ext_foreign_toplevel_handle_v1_listener handle_listener = {
	.closed = ignore_handle,
	.done = ignore_handle,
	.title = handle_title,
	.app_id = ignore_str,
	.identifier = ignore_str,
};

static void list_toplevel(void *data, struct ext_foreign_toplevel_list_v1 *l,
		struct ext_foreign_toplevel_handle_v1 *handle) {
	ext_foreign_toplevel_handle_v1_add_listener(handle, &handle_listener, NULL);
}

static void list_finished(void *data, struct ext_foreign_toplevel_list_v1 *l) {
}

static const struct ext_foreign_toplevel_list_v1_listener list_listener = {
	.toplevel = list_toplevel,
	.finished = list_finished,
};

static void global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version) {
	if (!strcmp(interface, wl_shm_interface.name)) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (!strcmp(interface, ext_foreign_toplevel_list_v1_interface.name)) {
		list = wl_registry_bind(registry, name, &ext_foreign_toplevel_list_v1_interface, 1);
		ext_foreign_toplevel_list_v1_add_listener(list, &list_listener, NULL);
	} else if (!strcmp(interface,
			ext_foreign_toplevel_image_capture_source_manager_v1_interface.name)) {
		sources = wl_registry_bind(registry, name,
			&ext_foreign_toplevel_image_capture_source_manager_v1_interface, 1);
	} else if (!strcmp(interface, ext_image_copy_capture_manager_v1_interface.name)) {
		copier = wl_registry_bind(registry, name, &ext_image_copy_capture_manager_v1_interface,
			1);
	}
}

static void global_remove(void *data, struct wl_registry *registry, uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {global, global_remove};

static void session_size(void *data, struct ext_image_copy_capture_session_v1 *s, uint32_t w,
		uint32_t h) {
	width = (int)w;
	height = (int)h;
}

static void session_shm_format(void *data, struct ext_image_copy_capture_session_v1 *s,
		uint32_t f) {
	if (format == 0xffffffff || f == WL_SHM_FORMAT_XRGB8888 || f == WL_SHM_FORMAT_ARGB8888
			|| f == WL_SHM_FORMAT_XBGR8888 || f == WL_SHM_FORMAT_ABGR8888) {
		format = f;
	}
}

static void session_dmabuf_device(void *data, struct ext_image_copy_capture_session_v1 *s,
		struct wl_array *device) {
}

static void session_dmabuf_format(void *data, struct ext_image_copy_capture_session_v1 *s,
		uint32_t f, struct wl_array *modifiers) {
}

static void session_done(void *data, struct ext_image_copy_capture_session_v1 *s) {
	done = true;
}

static void session_stopped(void *data, struct ext_image_copy_capture_session_v1 *s) {
	stopped = true;
	if (ready < 0) {
		ready = 0;
	}
}

static const struct ext_image_copy_capture_session_v1_listener session_listener = {
	.buffer_size = session_size,
	.shm_format = session_shm_format,
	.dmabuf_device = session_dmabuf_device,
	.dmabuf_format = session_dmabuf_format,
	.done = session_done,
	.stopped = session_stopped,
};

static void frame_transform(void *data, struct ext_image_copy_capture_frame_v1 *f,
		uint32_t t) {
}

static void frame_damage(void *data, struct ext_image_copy_capture_frame_v1 *f, int32_t x,
		int32_t y, int32_t w, int32_t h) {
}

static void frame_time(void *data, struct ext_image_copy_capture_frame_v1 *f, uint32_t hi,
		uint32_t lo, uint32_t ns) {
}

static void frame_ready(void *data, struct ext_image_copy_capture_frame_v1 *f) {
	ready = 1;
}

static void frame_failed(void *data, struct ext_image_copy_capture_frame_v1 *f,
		uint32_t reason) {
	printf("frame failed: %u\n", reason);
	ready = 0;
}

static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
	.transform = frame_transform,
	.damage = frame_damage,
	.presentation_time = frame_time,
	.ready = frame_ready,
	.failed = frame_failed,
};

// an X window, red, titled capme
static void x_window(void) {
	xcb_connection_t *c = x = xcb_connect(NULL, NULL);
	xcb_screen_t *s = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
	xcb_window_t w = xcb_generate_id(c);
	xcb_create_window(c, XCB_COPY_FROM_PARENT, w, s->root, 0, 0, 400, 300, 0,
		XCB_WINDOW_CLASS_INPUT_OUTPUT, s->root_visual, 0, NULL);
	xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, 5,
		"capme");
	xcb_map_window(c, w);
	free(xcb_get_input_focus_reply(c, xcb_get_input_focus(c), NULL));
	nanosleep(&(struct timespec){0, 500000000}, NULL);
	xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(c, xcb_get_geometry(c, w), NULL);
	xcb_gcontext_t gc = xcb_generate_id(c);
	uint32_t red = 0xff0000;
	xcb_create_gc(c, gc, w, XCB_GC_FOREGROUND, &red);
	xcb_rectangle_t all = {0, 0, g ? g->width : 400, g ? g->height : 300};
	xcb_poly_fill_rectangle(c, w, gc, 1, &all);
	free(g);
	free(xcb_get_input_focus_reply(c, xcb_get_input_focus(c), NULL));
	// the connection stays open: the window lives till the test's done
}

int main(int argc, char **argv) {
	if (argc < 2) {
		return 2;
	}
	if (argc > 2) {
		want_title = argv[2];
	}
	// a window that's there already, by title, else an X one of its own
	bool own = argc < 3;
	if (own) {
		x_window();
	}
	nanosleep(&(struct timespec){0, 500000000}, NULL);
	char cmd[512];
	snprintf(cmd, sizeof(cmd), "%s workspace 2 > /dev/null", argv[1]);
	if (system(cmd) != 0) {
		return 1;
	}
	nanosleep(&(struct timespec){0, 500000000}, NULL);

	struct wl_display *display = wl_display_connect(NULL);
	if (!display) {
		return 2;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	wl_display_roundtrip(display);
	printf("protocols: %s\n", shm && list && sources && copier ? "yes" : "no");
	printf("listed: %s\n", wanted ? "yes" : "no");
	if (!wanted || !sources || !copier) {
		return 1;
	}
	struct ext_image_capture_source_v1 *source =
		ext_foreign_toplevel_image_capture_source_manager_v1_create_source(sources, wanted);
	struct ext_image_copy_capture_session_v1 *session =
		ext_image_copy_capture_manager_v1_create_session(copier, source, 0);
	ext_image_copy_capture_session_v1_add_listener(session, &session_listener, NULL);
	while (!done && ready != 0 && wl_display_dispatch(display) != -1) {
	}
	printf("size %dx%d\n", width, height);
	if (!done || width <= 0 || height <= 0 || format == 0xffffffff) {
		return 1;
	}
	size_t stride = (size_t)width * 4, size = stride * (size_t)height;
	int fd = memfd_create("capture", 0);
	if (fd < 0 || ftruncate(fd, (off_t)size) != 0) {
		return 1;
	}
	uint8_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, width, height, (int32_t)stride,
		format);
	struct ext_image_copy_capture_frame_v1 *frame =
		ext_image_copy_capture_session_v1_create_frame(session);
	ext_image_copy_capture_frame_v1_add_listener(frame, &frame_listener, NULL);
	ext_image_copy_capture_frame_v1_attach_buffer(frame, buffer);
	ext_image_copy_capture_frame_v1_damage_buffer(frame, 0, 0, width, height);
	ext_image_copy_capture_frame_v1_capture(frame);
	while (ready < 0 && wl_display_dispatch(display) != -1) {
	}
	if (ready == 1) {
		// little endian: xrgb is b g r x in memory, xbgr r g b x
		uint8_t *p = pixels + (size_t)(height / 2) * stride + (size_t)(width / 2) * 4;
		bool bgr = format == WL_SHM_FORMAT_XBGR8888 || format == WL_SHM_FORMAT_ABGR8888;
		int r = bgr ? p[0] : p[2], g = p[1], b = bgr ? p[2] : p[0];
		// anything drawn at all: a window that isn't there comes out clear
		size_t lit = 0;
		for (size_t i = 0; i < size; i += 4) {
			lit += pixels[i] | pixels[i + 1] | pixels[i + 2] | pixels[i + 3] ? 1 : 0;
		}
		printf("drawn pixels: %zu of %zu\n", lit, size / 4);
		printf("hidden window captured: %s (%d %d %d)\n",
			r > 200 && g < 60 && b < 60 ? "red" : "other", r, g, b);
	}
	// a window that doesn't change still sends frames: the stream gets a
	// picture even when its first ones went while it was being set up
	ready = -1;
	frame = ext_image_copy_capture_session_v1_create_frame(session);
	ext_image_copy_capture_frame_v1_add_listener(frame, &frame_listener, NULL);
	ext_image_copy_capture_frame_v1_attach_buffer(frame, buffer);
	ext_image_copy_capture_frame_v1_capture(frame);
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (ready < 0 && wl_display_dispatch(display) != -1) {
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
	printf("unchanged window, next frame: %s\n", ready == 1 && ms < 1500 ? "yes" : "no");
	if (!own) {
		wl_display_disconnect(display);
		return 0;
	}
	// the window closes while it's shared: the session stops, nothing breaks
	xcb_disconnect(x);
	for (int i = 0; i < 50 && !stopped; i++) {
		wl_display_roundtrip(display);
		nanosleep(&(struct timespec){0, 20000000}, NULL);
	}
	printf("closed while shared, session stopped: %s\n", stopped ? "yes" : "no");
	wl_display_disconnect(display);
	return ready == 1 ? 0 : 1;
}
