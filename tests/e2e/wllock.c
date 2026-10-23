// wllock: a locker for the tests (ext-session-lock-v1), red on every screen.
//   wllock unlock MS  locks, says "locked", the keys it gets ("key N"),
//                     unlocks after MS and says "unlocked"
//   wllock crash      locks, says "locked", then quits without unlocking
//   wllock second     tries to lock: "locked", or "finished" turned away

#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "ext-session-lock-v1-client-protocol.h"

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct ext_session_lock_manager_v1 *manager;
static struct wl_seat *seat;
static struct wl_output *outputs[8];
static int outputs_len;
static bool locked, finished;

static void global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version) {
	if (!strcmp(interface, wl_compositor_interface.name)) {
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (!strcmp(interface, wl_shm_interface.name)) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (!strcmp(interface, ext_session_lock_manager_v1_interface.name)) {
		manager = wl_registry_bind(registry, name, &ext_session_lock_manager_v1_interface, 1);
	} else if (!strcmp(interface, wl_seat_interface.name)) {
		seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
	} else if (!strcmp(interface, wl_output_interface.name) && outputs_len < 8) {
		outputs[outputs_len++] = wl_registry_bind(registry, name, &wl_output_interface, 1);
	}
}

static void global_remove(void *data, struct wl_registry *registry, uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {global, global_remove};

// a red picture w x h
static struct wl_buffer *red(int w, int h) {
	int stride = w * 4, size = stride * h;
	int fd = memfd_create("wllock", 0);
	if (fd < 0 || ftruncate(fd, size) < 0) {
		return NULL;
	}
	uint32_t *pixels = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	for (int i = 0; i < w * h; i++) {
		pixels[i] = 0xffff0000;
	}
	munmap(pixels, (size_t)size);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
		WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	return buffer;
}

static void surface_configure(void *data, struct ext_session_lock_surface_v1 *lock_surface,
		uint32_t serial, uint32_t width, uint32_t height) {
	struct wl_surface *surface = data;
	ext_session_lock_surface_v1_ack_configure(lock_surface, serial);
	wl_surface_attach(surface, red((int)width, (int)height), 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(surface);
}

static const struct ext_session_lock_surface_v1_listener surface_listener = {
	surface_configure,
};

static void lock_locked(void *data, struct ext_session_lock_v1 *lock) {
	locked = true;
	printf("locked\n");
	fflush(stdout);
}

static void lock_finished(void *data, struct ext_session_lock_v1 *lock) {
	finished = true;
	printf("finished\n");
	fflush(stdout);
}

static const struct ext_session_lock_v1_listener lock_listener = {lock_locked, lock_finished};

static void key_keymap(void *data, struct wl_keyboard *k, uint32_t format, int32_t fd,
		uint32_t size) {
	close(fd);
}

static void key_enter(void *data, struct wl_keyboard *k, uint32_t serial,
		struct wl_surface *surface, struct wl_array *keys) {
}

static void key_leave(void *data, struct wl_keyboard *k, uint32_t serial,
		struct wl_surface *surface) {
}

static void key_key(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t time,
		uint32_t key, uint32_t state) {
	if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		printf("key %u\n", key);
		fflush(stdout);
	}
}

static void key_modifiers(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t depressed,
		uint32_t latched, uint32_t locked_mods, uint32_t group) {
}

static const struct wl_keyboard_listener keyboard_listener = {
	key_keymap, key_enter, key_leave, key_key, key_modifiers,
};

static double now(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

// the display's events for ms, or till done is set
static void run(struct wl_display *display, int ms, bool *done) {
	double end = now() + ms / 1000.0;
	while ((!done || !*done) && now() < end) {
		wl_display_flush(display);
		struct pollfd pfd = {wl_display_get_fd(display), POLLIN, 0};
		if (poll(&pfd, 1, (int)((end - now()) * 1000) + 1) > 0) {
			if (wl_display_dispatch(display) < 0) {
				return;
			}
		} else {
			wl_display_dispatch_pending(display);
		}
	}
}

int main(int argc, char **argv) {
	const char *mode = argc > 1 ? argv[1] : "";
	struct wl_display *display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "wllock: no display\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	if (!compositor || !shm || !manager) {
		fprintf(stderr, "wllock: no ext_session_lock_manager_v1\n");
		return 1;
	}
	if (seat) {
		struct wl_keyboard *keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
	}
	struct ext_session_lock_v1 *lock = ext_session_lock_manager_v1_lock(manager);
	ext_session_lock_v1_add_listener(lock, &lock_listener, NULL);
	for (int i = 0; i < outputs_len; i++) {
		struct wl_surface *surface = wl_compositor_create_surface(compositor);
		struct ext_session_lock_surface_v1 *lock_surface =
			ext_session_lock_v1_get_lock_surface(lock, surface, outputs[i]);
		ext_session_lock_surface_v1_add_listener(lock_surface, &surface_listener, surface);
	}
	if (!strcmp(mode, "second")) {
		double end = now() + 1.5;
		while (!locked && !finished && now() < end) {
			run(display, 50, NULL);
		}
		return 0;
	}
	run(display, 3000, &locked);
	if (!locked) {
		fprintf(stderr, "wllock: never locked\n");
		return 1;
	}
	if (!strcmp(mode, "crash")) {
		_exit(0); // gone without a word
	}
	run(display, argc > 2 ? atoi(argv[2]) : 1000, NULL);
	ext_session_lock_v1_unlock_and_destroy(lock);
	wl_display_roundtrip(display);
	printf("unlocked\n");
	fflush(stdout);
	return 0;
}
