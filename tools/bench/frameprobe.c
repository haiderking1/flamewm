// frameprobe: how well a wayland compositor keeps up, from a client's side.
// a small window that redraws on every frame callback, and asks
// wp_presentation when each frame reached the screen. any compositor, so
// flame and others can be compared with the same numbers:
// - missed: refreshes where our frame should have shown and didn't (two
//   presentations further apart than a refresh and a half)
// - latency: commit -> on screen
// usage: frameprobe [seconds] [-v]   -v: every miss with its timeline

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "presentation-time-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define WIDTH 320
#define HEIGHT 240
#define BUFFERS 3

struct buffer {
	struct wl_buffer *wl;
	uint32_t *pixels;
	bool busy;
};

struct pending {
	int64_t callback, commit; // ns: the frame callback this frame answered
	bool used;
};

static struct {
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct xdg_wm_base *wm_base;
	struct wp_presentation *presentation;
	clockid_t clock;
	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *toplevel;
	bool configured, closed;
	struct buffer buffers[BUFFERS];
	uint32_t frame;

	struct pending pending[64];
	int64_t last_callback; // when the frame callback we're drawing for came
	bool verbose;
	int64_t last_present, last_commit;
	int64_t refresh;
	uint64_t presented, discarded, missed, late_buffer;
	double lat_sum, lat_min, lat_max;
	int64_t start, end;
} probe = {.clock = CLOCK_MONOTONIC, .lat_min = 1e9};

static int64_t now_ns(void) {
	struct timespec ts;
	clock_gettime(probe.clock, &ts);
	return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void buffer_release(void *data, struct wl_buffer *wl) {
	((struct buffer *)data)->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {.release = buffer_release};

static bool make_buffers(void) {
	size_t size = (size_t)WIDTH * HEIGHT * 4;
	int fd = memfd_create("frameprobe", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t)(size * BUFFERS)) < 0) {
		return false;
	}
	uint8_t *data = mmap(NULL, size * BUFFERS, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		return false;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(probe.shm, fd, (int32_t)(size * BUFFERS));
	for (int i = 0; i < BUFFERS; i++) {
		probe.buffers[i].pixels = (uint32_t *)(data + size * i);
		probe.buffers[i].wl = wl_shm_pool_create_buffer(pool, (int32_t)(size * i), WIDTH,
			HEIGHT, WIDTH * 4, WL_SHM_FORMAT_XRGB8888);
		wl_buffer_add_listener(probe.buffers[i].wl, &buffer_listener, &probe.buffers[i]);
	}
	wl_shm_pool_destroy(pool);
	close(fd);
	return true;
}

static void feedback_sync_output(void *data, struct wp_presentation_feedback *fb,
		struct wl_output *output) {
}

static void feedback_presented(void *data, struct wp_presentation_feedback *fb,
		uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec, uint32_t refresh,
		uint32_t seq_hi, uint32_t seq_lo, uint32_t flags) {
	struct pending *p = data;
	int64_t when = ((int64_t)(((uint64_t)tv_sec_hi << 32) | tv_sec_lo)) * 1000000000LL + tv_nsec;
	if (refresh) {
		probe.refresh = refresh;
	}
	if (probe.last_present && probe.refresh && when > probe.last_present) {
		int64_t gap = when - probe.last_present;
		if (gap * 2 > probe.refresh * 3) {
			probe.missed += (uint64_t)((gap + probe.refresh / 2) / probe.refresh) - 1;
			if (probe.verbose && probe.start) {
				// times relative to the vblank that went without our frame
				int64_t missed_at = probe.last_present + probe.refresh;
				printf("  miss at %lld.%06llds: callback %+.3fms commit %+.3fms shown %+.3fms "
					"(prev commit %+.3fms)\n",
					(long long)(missed_at / 1000000000), (long long)(missed_at % 1000000000 / 1000),
					(p->callback - missed_at) / 1e6, (p->commit - missed_at) / 1e6,
					(when - missed_at) / 1e6, (probe.last_commit - missed_at) / 1e6);
			}
		}
	}
	probe.last_present = when;
	probe.last_commit = p->commit;
	probe.presented++;
	double lat = (double)(when - p->commit) / 1e6;
	probe.lat_sum += lat;
	if (lat < probe.lat_min) {
		probe.lat_min = lat;
	}
	if (lat > probe.lat_max) {
		probe.lat_max = lat;
	}
	p->used = false;
	wp_presentation_feedback_destroy(fb);
}

static void feedback_discarded(void *data, struct wp_presentation_feedback *fb) {
	((struct pending *)data)->used = false;
	probe.discarded++;
	wp_presentation_feedback_destroy(fb);
}

static const struct wp_presentation_feedback_listener feedback_listener = {
	.sync_output = feedback_sync_output,
	.presented = feedback_presented,
	.discarded = feedback_discarded,
};

static void draw(void);

static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
	wl_callback_destroy(cb);
	probe.last_callback = now_ns();
	draw();
}

static const struct wl_callback_listener frame_listener = {.done = frame_done};

static void draw(void) {
	struct buffer *buffer = NULL;
	for (int i = 0; i < BUFFERS; i++) {
		if (!probe.buffers[i].busy) {
			buffer = &probe.buffers[i];
			break;
		}
	}
	struct wl_callback *cb = wl_surface_frame(probe.surface);
	wl_callback_add_listener(cb, &frame_listener, NULL);
	if (!buffer) {
		// the compositor holds all of them: no frame from us this time
		probe.late_buffer++;
		wl_surface_commit(probe.surface);
		return;
	}
	// a moving bar, so every frame is really different
	uint32_t color = 0xff000000 | ((probe.frame * 7) & 0xff) << 16 | 0x4080;
	for (int i = 0; i < WIDTH * HEIGHT; i++) {
		buffer->pixels[i] = (i % WIDTH) == (int)(probe.frame % WIDTH) ? 0xffffffff : color;
	}
	probe.frame++;
	buffer->busy = true;
	wl_surface_attach(probe.surface, buffer->wl, 0, 0);
	wl_surface_damage_buffer(probe.surface, 0, 0, WIDTH, HEIGHT);
	struct pending *p = NULL;
	for (size_t i = 0; i < sizeof(probe.pending) / sizeof(probe.pending[0]); i++) {
		if (!probe.pending[i].used) {
			p = &probe.pending[i];
			break;
		}
	}
	if (p && probe.presentation) {
		p->used = true;
		struct wp_presentation_feedback *fb =
			wp_presentation_feedback(probe.presentation, probe.surface);
		wp_presentation_feedback_add_listener(fb, &feedback_listener, p);
	}
	if (p) {
		p->callback = probe.last_callback;
		p->commit = now_ns();
	}
	wl_surface_commit(probe.surface);
}

static void presentation_clock_id(void *data, struct wp_presentation *presentation,
		uint32_t clk_id) {
	probe.clock = (clockid_t)clk_id;
}

static const struct wp_presentation_listener presentation_listener = {
	.clock_id = presentation_clock_id,
};

static void wm_base_ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
	xdg_wm_base_pong(base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {.ping = wm_base_ping};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version) {
	if (!strcmp(interface, wl_compositor_interface.name)) {
		probe.compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (!strcmp(interface, wl_shm_interface.name)) {
		probe.shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (!strcmp(interface, xdg_wm_base_interface.name)) {
		probe.wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(probe.wm_base, &wm_base_listener, NULL);
	} else if (!strcmp(interface, wp_presentation_interface.name)) {
		probe.presentation = wl_registry_bind(registry, name, &wp_presentation_interface, 1);
		wp_presentation_add_listener(probe.presentation, &presentation_listener, NULL);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static void xdg_surface_configure(void *data, struct xdg_surface *surface, uint32_t serial) {
	xdg_surface_ack_configure(surface, serial);
	if (!probe.configured) {
		probe.configured = true;
		draw();
	}
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t width,
		int32_t height, struct wl_array *states) {
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel) {
	probe.closed = true;
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
};

int main(int argc, char *argv[]) {
	double seconds = argc > 1 ? atof(argv[1]) : 10;
	probe.verbose = argc > 2 && !strcmp(argv[2], "-v");
	struct wl_display *display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "frameprobe: no wayland display\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	wl_display_roundtrip(display);
	if (!probe.compositor || !probe.shm || !probe.wm_base) {
		fprintf(stderr, "frameprobe: compositor lacks wl_compositor/wl_shm/xdg_wm_base\n");
		return 1;
	}
	if (!probe.presentation) {
		fprintf(stderr, "frameprobe: no wp_presentation, can't tell when frames showed\n");
		return 1;
	}
	if (!make_buffers()) {
		fprintf(stderr, "frameprobe: shm buffers: %s\n", strerror(errno));
		return 1;
	}
	probe.surface = wl_compositor_create_surface(probe.compositor);
	probe.xdg_surface = xdg_wm_base_get_xdg_surface(probe.wm_base, probe.surface);
	xdg_surface_add_listener(probe.xdg_surface, &xdg_surface_listener, NULL);
	probe.toplevel = xdg_surface_get_toplevel(probe.xdg_surface);
	xdg_toplevel_add_listener(probe.toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_title(probe.toplevel, "frameprobe");
	xdg_toplevel_set_app_id(probe.toplevel, "frameprobe");
	wl_surface_commit(probe.surface);

	// a second to settle (mapping, the first frames), then count
	int64_t settle = now_ns() + 1000000000LL;
	while (!probe.closed && now_ns() < settle && wl_display_dispatch(display) >= 0) {
	}
	probe.presented = probe.discarded = probe.missed = probe.late_buffer = 0;
	probe.lat_sum = 0;
	probe.lat_min = 1e9;
	probe.lat_max = 0;
	probe.start = now_ns();
	int64_t stop = probe.start + (int64_t)(seconds * 1e9);
	while (!probe.closed && now_ns() < stop && wl_display_dispatch(display) >= 0) {
	}
	probe.end = now_ns();

	double span = (double)(probe.end - probe.start) / 1e9;
	double hz = probe.refresh ? 1e9 / (double)probe.refresh : 0;
	printf("frameprobe: %.1fs at %.3fHz\n", span, hz);
	printf("  presented %llu (%.1f fps), expected ~%.0f\n", (unsigned long long)probe.presented,
		probe.presented / span, hz * span);
	printf("  missed vblanks %llu, discarded %llu, no free buffer %llu\n",
		(unsigned long long)probe.missed, (unsigned long long)probe.discarded,
		(unsigned long long)probe.late_buffer);
	if (probe.presented) {
		printf("  commit -> on screen ms: min %.3f mean %.3f max %.3f\n", probe.lat_min,
			probe.lat_sum / (double)probe.presented, probe.lat_max);
	}
	wl_display_disconnect(display);
	return 0;
}
