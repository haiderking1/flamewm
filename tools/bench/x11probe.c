// x11probe: frameprobe's twin for X11. a window that presents a new gpu
// buffer (DRI3, allocated through GBM) the moment the last one reached the
// screen, and reads Present's CompleteNotify for when that was (ust) and on
// which vblank (msc). the same numbers on any X server, flame's or xwayland:
// - missed: vblanks between two completes that should have been next to
//   each other, and presents that were skipped
// - latency: present request -> on screen
// usage: x11probe [seconds] [-v]   (uses $DISPLAY)

#define _GNU_SOURCE
#include <fcntl.h>
#include <gbm.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <xcb/dri3.h>
#include <xcb/present.h>
#include <xcb/xcb.h>

#define WIDTH 320
#define HEIGHT 240
#define BUFFERS 3

struct buffer {
	struct gbm_bo *bo;
	xcb_pixmap_t pixmap;
	bool busy; // presented, the server hasn't said it's idle yet
};

static struct {
	xcb_connection_t *conn;
	xcb_window_t window;
	xcb_special_event_t *events;
	struct buffer buffers[BUFFERS];
	uint32_t serial;
	int64_t requested[256]; // by serial % 256: when it was presented
	bool in_flight;
	bool verbose;

	uint64_t last_msc;
	int64_t last_ust;
	uint64_t presented, skipped, missed, copies, flips;
	double lat_sum, lat_min, lat_max;
	bool counting;
} probe = {.lat_min = 1e9};

static int64_t now_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static bool make_buffers(xcb_screen_t *screen) {
	xcb_dri3_open_cookie_t cookie = xcb_dri3_open(probe.conn, screen->root, 0);
	xcb_dri3_open_reply_t *reply = xcb_dri3_open_reply(probe.conn, cookie, NULL);
	if (!reply) {
		fprintf(stderr, "x11probe: DRI3Open failed\n");
		return false;
	}
	int fd = xcb_dri3_open_reply_fds(probe.conn, reply)[0];
	free(reply);
	struct gbm_device *gbm = gbm_create_device(fd);
	if (!gbm) {
		fprintf(stderr, "x11probe: no gbm device on the server's render node\n");
		return false;
	}
	for (int i = 0; i < BUFFERS; i++) {
		struct buffer *b = &probe.buffers[i];
		// linear when the driver allows it (nvidia doesn't), else whatever
		// tiling it picks: the modifier goes along to the server
		const uint32_t tries[] = {GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING,
			GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT, GBM_BO_USE_RENDERING};
		for (size_t t = 0; t < sizeof(tries) / sizeof(tries[0]) && !b->bo; t++) {
			b->bo = gbm_bo_create(gbm, WIDTH, HEIGHT, GBM_FORMAT_XRGB8888, tries[t]);
		}
		if (!b->bo) {
			fprintf(stderr, "x11probe: gbm_bo_create failed\n");
			return false;
		}
		// a different shade each, when the cpu may write it (not needed for
		// timing, presents count either way)
		uint32_t stride;
		void *map_data = NULL;
		uint32_t *pixels = gbm_bo_map(b->bo, 0, 0, WIDTH, HEIGHT, GBM_BO_TRANSFER_WRITE,
			&stride, &map_data);
		if (pixels) {
			for (int y = 0; y < HEIGHT; y++) {
				for (int x = 0; x < WIDTH; x++) {
					pixels[y * (stride / 4) + x] = 0xff000000u | (uint32_t)(0x40 * (i + 1)) << 8;
				}
			}
			gbm_bo_unmap(b->bo, map_data);
		}
		b->pixmap = xcb_generate_id(probe.conn);
		int planes = gbm_bo_get_plane_count(b->bo);
		uint32_t strides[4] = {0}, offsets[4] = {0};
		int32_t fds[4];
		for (int p = 0; p < planes && p < 4; p++) {
			strides[p] = gbm_bo_get_stride_for_plane(b->bo, p);
			offsets[p] = gbm_bo_get_offset(b->bo, p);
			fds[p] = gbm_bo_get_fd_for_plane(b->bo, p);
		}
		// DRI3 1.2: every plane with its modifier
		xcb_void_cookie_t c = xcb_dri3_pixmap_from_buffers_checked(probe.conn, b->pixmap,
			probe.window, (uint8_t)planes, WIDTH, HEIGHT, strides[0], offsets[0],
			strides[1], offsets[1], strides[2], offsets[2], strides[3], offsets[3], 24, 32,
			gbm_bo_get_modifier(b->bo), fds);
		xcb_generic_error_t *err = xcb_request_check(probe.conn, c);
		if (err) {
			fprintf(stderr, "x11probe: PixmapFromBuffer failed (error %d)\n", err->error_code);
			free(err);
			return false;
		}
	}
	return true;
}

static void present_next(void) {
	struct buffer *b = NULL;
	for (int i = 0; i < BUFFERS; i++) {
		if (!probe.buffers[(probe.serial + i) % BUFFERS].busy) {
			b = &probe.buffers[(probe.serial + i) % BUFFERS];
			break;
		}
	}
	if (!b) {
		return; // all held by the server, the next idle one goes
	}
	uint32_t serial = ++probe.serial;
	b->busy = true;
	probe.requested[serial % 256] = now_ns();
	// target_msc 0: as soon as it can, one in flight like a frame callback loop
	xcb_present_pixmap(probe.conn, probe.window, b->pixmap, serial, 0, 0, 0, 0, 0, 0, 0,
		XCB_PRESENT_OPTION_NONE, 0, 0, 0, 0, NULL);
	xcb_flush(probe.conn);
	probe.in_flight = true;
}

static void handle_complete(xcb_present_complete_notify_event_t *e) {
	if (e->kind != XCB_PRESENT_COMPLETE_KIND_PIXMAP) {
		return;
	}
	probe.in_flight = false;
	int64_t ust = (int64_t)e->ust * 1000;
	if (probe.counting) {
		if (e->mode == XCB_PRESENT_COMPLETE_MODE_SKIP) {
			probe.skipped++;
		} else {
			if (e->mode == XCB_PRESENT_COMPLETE_MODE_FLIP) {
				probe.flips++;
			} else {
				probe.copies++;
			}
			if (probe.last_msc && e->msc > probe.last_msc + 1) {
				probe.missed += e->msc - probe.last_msc - 1;
				if (probe.verbose) {
					printf("  miss: msc %llu -> %llu, presented %+.3fms before it showed\n",
						(unsigned long long)probe.last_msc, (unsigned long long)e->msc,
						(ust - probe.requested[e->serial % 256]) / 1e6);
				}
			}
			probe.presented++;
			double lat = (ust - probe.requested[e->serial % 256]) / 1e6;
			probe.lat_sum += lat;
			if (lat < probe.lat_min) {
				probe.lat_min = lat;
			}
			if (lat > probe.lat_max) {
				probe.lat_max = lat;
			}
		}
	}
	if (e->mode != XCB_PRESENT_COMPLETE_MODE_SKIP) {
		probe.last_msc = e->msc;
		probe.last_ust = ust;
	}
	present_next();
}

static void handle_idle(xcb_present_idle_notify_event_t *e) {
	for (int i = 0; i < BUFFERS; i++) {
		if (probe.buffers[i].pixmap == e->pixmap) {
			probe.buffers[i].busy = false;
		}
	}
	if (!probe.in_flight) {
		present_next();
	}
}

static void dispatch(void) {
	xcb_generic_event_t *ev;
	while ((ev = xcb_poll_for_special_event(probe.conn, probe.events))) {
		xcb_present_generic_event_t *ge = (xcb_present_generic_event_t *)ev;
		if (ge->evtype == XCB_PRESENT_EVENT_COMPLETE_NOTIFY) {
			handle_complete((xcb_present_complete_notify_event_t *)ev);
		} else if (ge->evtype == XCB_PRESENT_EVENT_IDLE_NOTIFY) {
			handle_idle((xcb_present_idle_notify_event_t *)ev);
		}
		free(ev);
	}
	while ((ev = xcb_poll_for_event(probe.conn))) {
		if (probe.verbose) {
			if (ev->response_type == 0) {
				xcb_generic_error_t *err = (xcb_generic_error_t *)ev;
				printf("  X error %d, request %d.%d\n", err->error_code, err->major_code,
					err->minor_code);
			} else if ((ev->response_type & 0x7f) == XCB_GE_GENERIC) {
				printf("  a generic event that wasn't ours (event id not matched)\n");
			}
		}
		free(ev);
	}
}

static void run_for(int64_t ns) {
	int64_t until = now_ns() + ns;
	struct pollfd pfd = {.fd = xcb_get_file_descriptor(probe.conn), .events = POLLIN};
	while (now_ns() < until && !xcb_connection_has_error(probe.conn)) {
		dispatch();
		int64_t left = (until - now_ns()) / 1000000;
		poll(&pfd, 1, left > 100 ? 100 : (int)(left > 0 ? left : 0));
	}
}

int main(int argc, char *argv[]) {
	double seconds = argc > 1 ? atof(argv[1]) : 10;
	probe.verbose = argc > 2 && !strcmp(argv[2], "-v");
	probe.conn = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(probe.conn)) {
		fprintf(stderr, "x11probe: can't connect to $DISPLAY\n");
		return 1;
	}
	xcb_present_query_version_reply_t *pv = xcb_present_query_version_reply(probe.conn,
		xcb_present_query_version(probe.conn, 1, 2), NULL);
	xcb_dri3_query_version_reply_t *dv = xcb_dri3_query_version_reply(probe.conn,
		xcb_dri3_query_version(probe.conn, 1, 2), NULL);
	if (!pv || !dv || dv->major_version < 1 || (dv->major_version == 1 && dv->minor_version < 2)) {
		fprintf(stderr, "x11probe: the X server lacks Present or DRI3 1.2\n");
		return 1;
	}
	free(pv);
	free(dv);

	xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(probe.conn)).data;
	probe.window = xcb_generate_id(probe.conn);
	uint32_t values[] = {screen->black_pixel};
	xcb_create_window(probe.conn, 24, probe.window, screen->root, 0, 0, WIDTH, HEIGHT, 0,
		XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, XCB_CW_BACK_PIXEL, values);
	const char *title = "x11probe";
	xcb_change_property(probe.conn, XCB_PROP_MODE_REPLACE, probe.window, XCB_ATOM_WM_NAME,
		XCB_ATOM_STRING, 8, (uint32_t)strlen(title), title);
	xcb_map_window(probe.conn, probe.window);

	uint32_t eid = xcb_generate_id(probe.conn);
	probe.events = xcb_register_for_special_xge(probe.conn, &xcb_present_id, eid, NULL);
	xcb_present_select_input(probe.conn, eid, probe.window,
		XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY | XCB_PRESENT_EVENT_MASK_IDLE_NOTIFY);
	xcb_flush(probe.conn);

	if (!make_buffers(screen)) {
		return 1;
	}
	present_next();
	run_for(1000000000LL); // mapping, the first frames
	probe.counting = true;
	int64_t start = now_ns();
	run_for((int64_t)(seconds * 1e9));
	double span = (now_ns() - start) / 1e9;

	printf("x11probe: %.1fs on %s\n", span, getenv("DISPLAY") ? getenv("DISPLAY") : "?");
	printf("  presented %llu (%.1f fps), %llu copies %llu flips\n",
		(unsigned long long)probe.presented, probe.presented / span,
		(unsigned long long)probe.copies, (unsigned long long)probe.flips);
	printf("  missed vblanks %llu, skipped %llu\n", (unsigned long long)probe.missed,
		(unsigned long long)probe.skipped);
	if (probe.presented) {
		printf("  present -> on screen ms: min %.3f mean %.3f max %.3f\n", probe.lat_min,
			probe.lat_sum / (double)probe.presented, probe.lat_max);
	}
	xcb_disconnect(probe.conn);
	return 0;
}
