#include "xshell/xshell.h"

#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <drm_fourcc.h>
#include <time.h>
#include <unistd.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>
#include <xf86drm.h>
#include <xkbcommon/xkbcommon.h>

#include "core/server.h"
#include "util/listener.h"
#include "util/timer.h"
#include "xbridge/xbridge.h"
#include "xserver/xserver.h"

#define MODE_COPY 0
#define MODE_SKIP 2
#define OPTION_ASYNC 1

// ---- client gpu buffers as textures ----

// a DRI3 pixmap's dmabuf as a wlr_buffer, the fds are its own copies
struct xdmabuf {
	struct wlr_buffer base;
	struct wlr_dmabuf_attributes attribs;
};

static void xdmabuf_destroy(struct wlr_buffer *buffer) {
	struct xdmabuf *xdmabuf = wl_container_of(buffer, xdmabuf, base);
	wlr_dmabuf_attributes_finish(&xdmabuf->attribs);
	free(xdmabuf);
}

static bool xdmabuf_get_dmabuf(struct wlr_buffer *buffer, struct wlr_dmabuf_attributes *out) {
	struct xdmabuf *xdmabuf = wl_container_of(buffer, xdmabuf, base);
	*out = xdmabuf->attribs;
	return true;
}

static const struct wlr_buffer_impl xdmabuf_impl = {
	.destroy = xdmabuf_destroy,
	.get_dmabuf = xdmabuf_get_dmabuf,
};

// a pixmap's texture, imported once: swapchains cycle the same few buffers.
// presents showing it hold a reference, the pixmap holds one while it lives
struct xpixmap {
	int refs;
	struct wlr_buffer *buffer;
	struct wlr_texture *texture;
	int width, height;
	bool opaque;
};

static void xpixmap_unref(struct xpixmap *xpixmap) {
	if (--xpixmap->refs > 0) {
		return;
	}
	wlr_texture_destroy(xpixmap->texture);
	wlr_buffer_drop(xpixmap->buffer);
	free(xpixmap);
}

static struct xpixmap *xpixmap_get(struct flame_xshell *xshell, struct xs_pixmap *pixmap) {
	if (pixmap->data) {
		return pixmap->data;
	}
	if (pixmap->planes == 0) {
		return NULL; // a core pixmap, nothing was ever drawn into it here
	}
	struct xdmabuf *xdmabuf = calloc(1, sizeof(*xdmabuf));
	if (!xdmabuf) {
		return NULL;
	}
	wlr_buffer_init(&xdmabuf->base, &xdmabuf_impl, pixmap->width, pixmap->height);
	struct wlr_dmabuf_attributes *attribs = &xdmabuf->attribs;
	attribs->width = pixmap->width;
	attribs->height = pixmap->height;
	attribs->format = pixmap->format;
	attribs->modifier = pixmap->modifier;
	attribs->n_planes = pixmap->planes;
	for (int i = 0; i < pixmap->planes; i++) {
		attribs->fd[i] = fcntl(pixmap->fds[i], F_DUPFD_CLOEXEC, 0);
		attribs->offset[i] = pixmap->offsets[i];
		attribs->stride[i] = pixmap->strides[i];
	}
	for (int i = pixmap->planes; i < WLR_DMABUF_MAX_PLANES; i++) {
		attribs->fd[i] = -1;
	}
	struct wlr_texture *texture =
		wlr_texture_from_buffer(xshell->server->render.renderer, &xdmabuf->base);
	if (!texture) {
		wlr_log(WLR_ERROR, "xshell: can't import a %dx%d X pixmap (format 0x%x modifier 0x%llx)",
			pixmap->width, pixmap->height, pixmap->format,
			(unsigned long long)pixmap->modifier);
		wlr_buffer_drop(&xdmabuf->base);
		return NULL;
	}
	struct xpixmap *xpixmap = calloc(1, sizeof(*xpixmap));
	if (!xpixmap) {
		wlr_texture_destroy(texture);
		wlr_buffer_drop(&xdmabuf->base);
		return NULL;
	}
	xpixmap->refs = 1; // the pixmap's own
	xpixmap->buffer = &xdmabuf->base;
	xpixmap->texture = texture;
	xpixmap->width = pixmap->width;
	xpixmap->height = pixmap->height;
	xpixmap->opaque = pixmap->depth != 32;
	pixmap->data = xpixmap;
	return xpixmap;
}

void xshell_pixmap_destroyed(struct flame_xshell *xshell, struct xs_pixmap *pixmap) {
	if (pixmap->data) {
		xpixmap_unref(pixmap->data);
		pixmap->data = NULL;
	}
}

static struct wlr_drm_syncobj_timeline *timeline_of(struct flame_xshell *xshell,
		struct xs_syncobj *syncobj) {
	if (!syncobj) {
		return NULL;
	}
	if (!syncobj->data) {
		int drm_fd = wlr_renderer_get_drm_fd(xshell->server->render.renderer);
		syncobj->data = drm_fd >= 0 ? wlr_drm_syncobj_timeline_import(drm_fd, syncobj->fd) : NULL;
	}
	return syncobj->data;
}

void xshell_syncobj_destroyed(struct flame_xshell *xshell, struct xs_syncobj *syncobj) {
	if (syncobj->data) {
		wlr_drm_syncobj_timeline_unref(syncobj->data);
		syncobj->data = NULL;
	}
}

// ---- presents ----

struct xpresent {
	struct wl_list link; // flame_xwindow.queue or .retiring
	struct flame_xwindow *window;
	uint32_t target_window; // the X window the app presented to
	uint32_t serial, pixmap_id, idle_fence;
	struct xpixmap *pixmap;
	int16_t x_off, y_off;
	struct wlr_drm_syncobj_timeline *acquire, *release;
	uint64_t acquire_point, release_point;
	int64_t requested; // ns, when the app handed it over
	uint64_t target; // the msc it's meant to be on screen at

	// the first frame that drew it, completion goes out when that one flips
	uint64_t drawn_id;
	struct flame_frame *frame;
	// the last frame that read it, the buffer is free once its gpu work is done
	struct wlr_drm_syncobj_timeline *render_done;
	uint64_t render_point;
	struct wlr_drm_syncobj_timeline_waiter waiter;
	bool waiting;

	bool completed, released;
	// replaced before anything drew it: completes as skipped at its msc
	bool skipped;
	// its acquire point hasn't signalled: it waits in the queue for the app's
	// gpu work, drawing it now would make our whole frame wait for it
	struct wlr_drm_syncobj_timeline_waiter acquire_waiter;
	bool acquiring;
	// on screen, the complete held back until held_until: when it made it
	// (done_when) and on which vblank (done_msc) go out unchanged then
	bool held;
	int64_t held_until, done_when;
	uint64_t done_msc;
};

static void stop_acquiring(struct xpresent *xp) {
	if (xp->acquiring) {
		wlr_drm_syncobj_timeline_waiter_finish(&xp->acquire_waiter);
		xp->acquiring = false;
	}
}

static void xpresent_free(struct xpresent *xp) {
	wl_list_remove(&xp->link);
	stop_acquiring(xp);
	if (xp->waiting) {
		wlr_drm_syncobj_timeline_waiter_finish(&xp->waiter);
	}
	xpixmap_unref(xp->pixmap);
	if (xp->acquire) {
		wlr_drm_syncobj_timeline_unref(xp->acquire);
	}
	if (xp->release) {
		wlr_drm_syncobj_timeline_unref(xp->release);
	}
	if (xp->render_done) {
		wlr_drm_syncobj_timeline_unref(xp->render_done);
	}
	free(xp);
}

static void maybe_free(struct xpresent *xp) {
	if (xp->completed && xp->released && xp != xp->window->shown) {
		xpresent_free(xp);
	}
}

// ---- the msc ----

// a NotifyMSC: the app wants to hear when msc reaches target
struct xnotify {
	struct wl_list link; // flame_xshell.notifies
	uint32_t window, serial;
	uint64_t target;
};

// no output to follow: a refresh a second, like Xorg's fake crtc
#define NO_OUTPUT_PERIOD 1000000000LL

static int64_t clock_period(const struct flame_xshell *xshell) {
	return xshell->clock.frame ? xshell->clock.frame->schedule.period : NO_OUTPUT_PERIOD;
}

// msc follows the clock output's vblank grid, which every flip anchors
// (its kernel timestamp, else when it was reported): it moves by the
// refreshes between anchors, and on by time when nothing flips
static void clock_sync(struct flame_xshell *xshell) {
	struct flame_frame *frame = xshell->clock.frame;
	if (!xshell->clock.when) {
		xshell->clock.when = frame && frame->schedule.have_vblank
			? frame->schedule.vblank : now_ns();
		xshell->clock.msc = 1;
		return;
	}
	if (!frame || !frame->schedule.have_vblank || frame->schedule.vblank <= xshell->clock.when) {
		return;
	}
	int64_t period = frame->schedule.period;
	// a flip is a vblank at least, whatever the period claims
	int64_t steps = (frame->schedule.vblank - xshell->clock.when + period / 2) / period;
	xshell->clock.msc += steps > 0 ? (uint64_t)steps : 1;
	xshell->clock.when = frame->schedule.vblank;
}

// the msc at time t, never going backwards
static uint64_t msc_at(struct flame_xshell *xshell, int64_t t) {
	clock_sync(xshell);
	uint64_t msc = xshell->clock.msc;
	if (t > xshell->clock.when) {
		msc += (uint64_t)((t - xshell->clock.when) / clock_period(xshell));
	}
	if (msc < xshell->clock.seen) {
		return xshell->clock.seen;
	}
	xshell->clock.seen = msc;
	return msc;
}

// the msc of a flip: its timestamp sits on a vblank, a hair before or after
// the predicted grid point, so it rounds to the nearest one. msc_at would
// floor a flip that came early to the vblank before it, and the app would
// see that msc twice and then a gap
static uint64_t msc_of_flip(struct flame_xshell *xshell, int64_t when) {
	clock_sync(xshell);
	uint64_t msc = xshell->clock.msc;
	int64_t period = clock_period(xshell);
	if (when > xshell->clock.when) {
		msc += (uint64_t)((when - xshell->clock.when + period / 2) / period);
	}
	if (msc < xshell->clock.seen) {
		return xshell->clock.seen;
	}
	xshell->clock.seen = msc;
	return msc;
}

// when the vblank for msc comes, on the grid
static int64_t msc_time(struct flame_xshell *xshell, uint64_t msc) {
	clock_sync(xshell);
	return xshell->clock.when
		+ ((int64_t)msc - (int64_t)xshell->clock.msc) * clock_period(xshell);
}

// the clock follows frame from now on (NULL: none), counting on from here
static void clock_follow(struct flame_xshell *xshell, struct flame_frame *frame) {
	int64_t now = now_ns();
	uint64_t msc = msc_at(xshell, now);
	xshell->clock.frame = frame;
	xshell->clock.msc = msc;
	xshell->clock.when = now;
}

// Present's rule: a target already gone by with a divisor moves to the next
// msc where msc % divisor == remainder. async goes now
static uint64_t present_target(uint64_t target, uint64_t divisor, uint64_t remainder,
		uint64_t now_msc) {
	if (divisor && now_msc >= target) {
		target = now_msc - now_msc % divisor + remainder;
		if (target <= now_msc) {
			target += divisor;
		}
	}
	return target;
}

static void complete(struct xpresent *xp, uint8_t mode, int64_t when, uint64_t msc) {
	if (xp->completed) {
		return;
	}
	xp->completed = true;
	struct flame_xshell *xshell = xp->window->xshell;
	xserver_present_complete(xshell->xserver, xp->target_window, xp->serial, false, mode,
		(uint64_t)(when / 1000), msc);
	maybe_free(xp);
}

// a held complete goes out: the app hears its frame made it, when and on which
// vblank, and starts the next one. woke: it was due (the app waits on it and
// its draw time is measured from now), not sent early because the app went on
static void deliver(struct xpresent *xp, bool woke) {
	struct flame_xwindow *window = xp->window;
	xp->held = false;
	if (woke) {
		window->woken = now_ns();
		window->drawing = true;
	}
	complete(xp, MODE_COPY, xp->done_when, xp->done_msc);
}

// the app presented again without waiting for its complete: no point holding
// what's left, it goes out now
static void deliver_held(struct flame_xwindow *window) {
	struct xpresent *xp, *tmp;
	if (window->shown && window->shown->held) {
		deliver(window->shown, false);
	}
	wl_list_for_each_safe(xp, tmp, &window->retiring, link) {
		if (xp->held) {
			deliver(xp, false);
		}
	}
}

// the app can have its buffer back: its release point goes off (explicit
// sync), its idle fence and IdleNotify (implicit)
static void released(struct xpresent *xp) {
	xp->released = true;
	if (xp->release) {
		wlr_drm_syncobj_timeline_signal(xp->release, xp->release_point);
	}
	struct flame_xshell *xshell = xp->window->xshell;
	xserver_present_idle(xshell->xserver, xp->target_window, xp->serial, xp->pixmap_id,
		xp->idle_fence);
	maybe_free(xp);
}

static void handle_render_done(struct wlr_drm_syncobj_timeline_waiter *waiter) {
	struct xpresent *xp = wl_container_of(waiter, xp, waiter);
	wlr_drm_syncobj_timeline_waiter_finish(&xp->waiter);
	xp->waiting = false;
	released(xp);
}

// replaced: free once the last frame that read it is done reading. waited on
// here and signalled from here: our timeline can live on another device fd
// than the app's syncobj (KMS vs render node), the kernel won't chain those
static void release_when_idle(struct flame_xshell *xshell, struct xpresent *xp) {
	if (xp->render_done && wlr_drm_syncobj_timeline_waiter_init(&xp->waiter,
			xp->render_done, xp->render_point, 0, xshell->server->loop, handle_render_done)) {
		xp->waiting = true;
		return;
	}
	released(xp);
}

static void retire(struct flame_xwindow *window, struct xpresent *xp) {
	stop_acquiring(xp);
	wl_list_insert(&window->retiring, &xp->link);
	if (!xp->drawn_id) {
		// never made it to the screen. the buffer goes back now, the complete
		// waits for its msc like on Xorg: apps (NVIDIA's) throttle on how
		// many are still out, a skip right away has them render the next one
		xp->skipped = true;
		uint64_t now_msc = msc_at(window->xshell, now_ns());
		if (xp->target <= now_msc) {
			complete(xp, MODE_SKIP, msc_time(window->xshell, now_msc), now_msc);
		}
	}
	release_when_idle(window->xshell, xp);
}

static void show(struct flame_xwindow *window, struct xpresent *xp) {
	struct xpresent *old = window->shown;
	window->shown = xp;
	wl_list_init(&xp->link);
	scene_node_set_position(&window->content->node, xp->x_off, xp->y_off);
	scene_buffer_set(window->content, xp->pixmap->texture, xp, xp->pixmap->width,
		xp->pixmap->height, xp->pixmap->opaque, xp->acquire, xp->acquire_point);
	if (old) {
		wl_list_remove(&old->link);
		retire(window, old);
	}
}

// waits for the refresh before its msc. one already queued for the same msc
// never makes it, like on Xorg
static void enqueue(struct flame_xwindow *window, struct xpresent *xp) {
	struct xpresent *queued, *tmp;
	wl_list_for_each_safe(queued, tmp, &window->queue, link) {
		if (queued->target == xp->target) {
			wl_list_remove(&queued->link);
			retire(window, queued);
		}
	}
	struct wl_list *before = &window->queue;
	wl_list_for_each(queued, &window->queue, link) {
		if (queued->target > xp->target) {
			break;
		}
		before = &queued->link;
	}
	wl_list_insert(before, &xp->link);
}

// a target this far out is the app's own idea of msc, it waits for flips
// to get there, no timer that far ahead
#define MSC_FAR (1ULL << 24)

static void arm_msc_timer(struct flame_xshell *xshell) {
	int64_t next = 0;
	uint64_t now_msc = msc_at(xshell, now_ns());
	struct flame_xwindow *window;
	wl_list_for_each(window, &xshell->windows, link) {
		struct xpresent *shown = window->shown;
		if (shown && shown->held) {
			next = !next || shown->held_until < next ? shown->held_until : next;
		}
		if (shown && !shown->completed && !shown->drawn_id && shown->target - now_msc < MSC_FAR) {
			int64_t at = msc_time(xshell, shown->target + 1);
			next = !next || at < next ? at : next;
		}
		struct xpresent *xp;
		wl_list_for_each(xp, &window->retiring, link) {
			if (xp->held) {
				next = !next || xp->held_until < next ? xp->held_until : next;
			}
			if (xp->skipped && !xp->completed && xp->target - now_msc < MSC_FAR) {
				int64_t at = msc_time(xshell, xp->target);
				next = !next || at < next ? at : next;
			}
		}
		if (wl_list_empty(&window->queue)) {
			continue;
		}
		struct xpresent *first = wl_container_of(window->queue.next, first, link);
		if (first->target - now_msc < MSC_FAR) {
			// at the refresh before: the frame after that one shows it
			int64_t at = msc_time(xshell, first->target - 1);
			next = !next || at < next ? at : next;
		}
	}
	struct xnotify *notify;
	wl_list_for_each(notify, &xshell->notifies, link) {
		if (notify->target - now_msc < MSC_FAR) {
			int64_t at = msc_time(xshell, notify->target);
			next = !next || at < next ? at : next;
		}
	}
	timer_arm(&xshell->msc_timer, next);
}

// queued presents whose msc is next go on screen, NotifyMSCs that reached
// theirs hear it. without a clock yet, everything is due
static void process_msc(struct flame_xshell *xshell, int64_t now) {
	uint64_t now_msc = msc_at(xshell, now);
	struct flame_xwindow *window;
	wl_list_for_each(window, &xshell->windows, link) {
		struct xpresent *xp, *tmp;
		if (window->shown && window->shown->held && window->shown->held_until <= now) {
			deliver(window->shown, true);
		}
		wl_list_for_each_safe(xp, tmp, &window->retiring, link) {
			if (xp->held && xp->held_until <= now) {
				deliver(xp, true);
			}
		}
		while (!wl_list_empty(&window->queue)) {
			struct xpresent *first = wl_container_of(window->queue.next, first, link);
			if (first->acquiring || first->target > now_msc + 1) {
				break; // the ones behind it wait too, order stays
			}
			wl_list_remove(&first->link);
			show(window, first);
		}
		// a refresh past its msc and nothing drew it: the window is covered or
		// on another workspace. it completes anyway like a copy on Xorg, or a
		// FIFO app waits on it forever
		struct xpresent *shown = window->shown;
		if (shown && !shown->completed && !shown->drawn_id && shown->target < now_msc) {
			complete(shown, MODE_COPY, msc_time(xshell, now_msc), now_msc);
		}
		wl_list_for_each_safe(xp, tmp, &window->retiring, link) {
			if (xp->skipped && !xp->completed && xp->target <= now_msc) {
				complete(xp, MODE_SKIP, msc_time(xshell, now_msc), now_msc);
			}
		}
	}
	struct xnotify *notify, *tmp;
	wl_list_for_each_safe(notify, tmp, &xshell->notifies, link) {
		if (notify->target > now_msc) {
			continue;
		}
		xserver_present_complete(xshell->xserver, notify->window, notify->serial, true,
			MODE_COPY, (uint64_t)(msc_time(xshell, now_msc) / 1000), now_msc);
		wl_list_remove(&notify->link);
		free(notify);
	}
	arm_msc_timer(xshell);
}

static void handle_msc_timer(void *data) {
	process_msc(data, now_ns());
}

static void handle_acquired(struct wlr_drm_syncobj_timeline_waiter *waiter) {
	struct xpresent *xp = wl_container_of(waiter, xp, acquire_waiter);
	stop_acquiring(xp);
	process_msc(xp->window->xshell, now_ns());
}

// the app's gpu work for it is done. if not, a wait starts: false until then
static bool acquired(struct xpresent *xp) {
	bool done = false;
	if (!wlr_drm_syncobj_timeline_check(xp->acquire, xp->acquire_point,
			DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, &done) || done) {
		return true; // can't tell, or done: show it
	}
	xp->acquiring = wlr_drm_syncobj_timeline_waiter_init(&xp->acquire_waiter, xp->acquire,
		xp->acquire_point, 0, xp->window->xshell->server->loop, handle_acquired);
	return !xp->acquiring;
}

static struct flame_xwindow *find_window(struct flame_xshell *xshell, uint32_t id) {
	struct flame_xwindow *window;
	wl_list_for_each(window, &xshell->windows, link) {
		if (window->id == id) {
			return window;
		}
	}
	return NULL;
}

// the flame window of the top-level an X window is in
static struct flame_xwindow *window_of(struct flame_xshell *xshell, uint32_t id) {
	struct xs_window *window = xserver_window(xshell->xserver, id);
	while (window && window->parent && !xs_window_top_level(window)) {
		window = window->parent;
	}
	return window ? find_window(xshell, window->id) : NULL;
}

void xshell_present(struct flame_xshell *xshell, const struct xs_present *present) {
	struct flame_xwindow *window = window_of(xshell, present->window);
	if (!window) {
		// mapped in the same breath, the window shows up first
		xshell_windows_changed(xshell);
		window = window_of(xshell, present->window);
	}
	if (window) {
		// woken by its complete: that's its draw time. an app idle for a while
		// (over 50ms) isn't drawing frame after frame, that's no sample
		if (window->drawing) {
			int64_t took = now_ns() - window->woken;
			if (took < 50000000LL) {
				window->draw_time = took > window->draw_time
					? took : window->draw_time + (took - window->draw_time) / 8;
			}
			window->drawing = false;
		}
		deliver_held(window);
	}
	struct xpixmap *xpixmap = xpixmap_get(xshell, present->pixmap);
	struct wlr_drm_syncobj_timeline *acquire = timeline_of(xshell, present->acquire);
	struct wlr_drm_syncobj_timeline *release = timeline_of(xshell, present->release);
	if (!window || !xpixmap || (present->acquire && !acquire)) {
		// nowhere to show it: skipped, and the app gets its buffer back
		if (release) {
			wlr_drm_syncobj_timeline_signal(release, present->release_point);
		}
		xserver_present_complete(xshell->xserver, present->window, present->serial, false,
			MODE_SKIP, (uint64_t)(now_ns() / 1000), msc_at(xshell, now_ns()));
		xserver_present_idle(xshell->xserver, present->window, present->serial,
			present->pixmap->id, present->idle_fence);
		return;
	}
	struct xpresent *xp = calloc(1, sizeof(*xp));
	if (!xp) {
		return;
	}
	xp->window = window;
	xp->target_window = present->window;
	xp->serial = present->serial;
	xp->pixmap_id = present->pixmap->id;
	xp->idle_fence = present->idle_fence;
	xp->pixmap = xpixmap;
	xpixmap->refs++;
	xp->x_off = present->x_off;
	xp->y_off = present->y_off;
	xp->acquire = acquire ? wlr_drm_syncobj_timeline_ref(acquire) : NULL;
	xp->release = release ? wlr_drm_syncobj_timeline_ref(release) : NULL;
	xp->acquire_point = present->acquire_point;
	xp->release_point = present->release_point;
	xp->requested = now_ns();
	// due by the next flip: shown at the next frame, a second one before that
	// frame replaces it (skipped). later: queued until the refresh before its
	// msc. that's what paces FIFO apps, they wait for their complete msc
	uint64_t now_msc = msc_at(xshell, xp->requested);
	xp->target = present->options & OPTION_ASYNC ? now_msc
		: present_target(present->target_msc, present->divisor, present->remainder, now_msc);
	if (xp->acquire && !acquired(xp)) {
		enqueue(window, xp);
	} else if (xp->target <= now_msc + 1 && wl_list_empty(&window->queue)) {
		show(window, xp);
	} else {
		enqueue(window, xp);
		process_msc(xshell, xp->requested);
	}
	// whatever it replaced completes at its msc
	arm_msc_timer(xshell);
}

void xshell_notify_msc(struct flame_xshell *xshell, uint32_t window, uint32_t serial,
		uint64_t target_msc, uint64_t divisor, uint64_t remainder) {
	int64_t now = now_ns();
	uint64_t now_msc = msc_at(xshell, now);
	uint64_t target = present_target(target_msc, divisor, remainder, now_msc);
	if (target <= now_msc) {
		xserver_present_complete(xshell->xserver, window, serial, true, MODE_COPY,
			(uint64_t)(msc_time(xshell, now_msc) / 1000), now_msc);
		return;
	}
	struct xnotify *notify = calloc(1, sizeof(*notify));
	if (!notify) {
		return;
	}
	notify->window = window;
	notify->serial = serial;
	notify->target = target;
	wl_list_insert(xshell->notifies.prev, &notify->link);
	arm_msc_timer(xshell);
}

// ---- following frames ----

struct xshell_frame {
	struct wl_list link; // flame_xshell.frames
	struct flame_xshell *xshell;
	struct flame_frame *frame;
	struct wl_listener presented;
	struct wl_listener destroy;
};

#define JIT_MARGIN_NS 1000000LL

// when to tell an app its frame made it so its next present lands right
// before the render that can show it: its draw time and room for that to
// vary before render_at. 0 for right away: draw time unknown, or no render
static int64_t app_wake(const struct flame_xwindow *window, int64_t render_at) {
	if (!render_at || window->draw_time <= 0) {
		return 0;
	}
	int64_t margin = window->draw_time / 2 > JIT_MARGIN_NS ? window->draw_time / 2 : JIT_MARGIN_NS;
	return render_at - window->draw_time - margin;
}

static void check_presented(struct flame_xshell *xshell, struct xpresent *xp,
		struct flame_frame *frame, const struct flame_frame_presented *event) {
	if (xp->completed || !xp->drawn_id || xp->frame != frame || xp->drawn_id > event->draw_id) {
		return;
	}
	if (!event->presented) {
		return; // that frame was dropped, the next one draws it again
	}
	// buffer handed over -> on screen, next to the wayland one in frame timing
	timing_latency(&frame->timing, true, event->when - xp->requested);
	xp->done_when = event->when;
	xp->done_msc = msc_of_flip(xshell, event->when);
	// FIFO apps start their next frame when they hear this one made it: told
	// at the flip, that frame waits a whole refresh for flame's next render.
	// told just in time, it makes that render, like wayland's frame callbacks
	int64_t now = now_ns();
	int64_t wake = app_wake(xp->window, schedule_render_at(&frame->schedule, now));
	if (wake > now) {
		xp->held = true;
		xp->held_until = wake;
		return; // handle_presented arms the timer for it
	}
	deliver(xp, true);
}

static void handle_presented(struct wl_listener *listener, void *data) {
	struct xshell_frame *xframe = wl_container_of(listener, xframe, presented);
	const struct flame_frame_presented *event = data;
	struct flame_xshell *xshell = xframe->xshell;
	struct flame_xwindow *window;
	wl_list_for_each(window, &xshell->windows, link) {
		struct xpresent *xp, *tmp;
		wl_list_for_each_safe(xp, tmp, &window->retiring, link) {
			check_presented(xframe->xshell, xp, xframe->frame, event);
		}
		if (window->shown) {
			check_presented(xframe->xshell, window->shown, xframe->frame, event);
		}
	}
	// the msc moved: queued presents and NotifyMSCs may be due
	process_msc(xshell, now_ns());
}

static void handle_frame_destroy(struct wl_listener *listener, void *data) {
	struct xshell_frame *xframe = wl_container_of(listener, xframe, destroy);
	// whatever was waiting on that output won't hear from it again
	struct flame_xwindow *window;
	wl_list_for_each(window, &xframe->xshell->windows, link) {
		struct xpresent *xp, *tmp;
		wl_list_for_each_safe(xp, tmp, &window->retiring, link) {
			if (xp->frame == xframe->frame) {
				xp->frame = NULL;
				if (xp->held) {
					deliver(xp, false); // it made it, just not told yet
				} else {
					complete(xp, MODE_SKIP, now_ns(), msc_at(xframe->xshell, now_ns()));
				}
			}
		}
		if (window->shown && window->shown->held && window->shown->frame == xframe->frame) {
			deliver(window->shown, false);
		}
		if (window->shown && window->shown->frame == xframe->frame) {
			window->shown->frame = NULL;
			window->shown->drawn_id = 0; // drawn again on another output
		}
	}
	if (xframe->xshell->clock.frame == xframe->frame) {
		// another output takes over, counting on from here
		struct xshell_frame *other;
		struct flame_frame *next = NULL;
		wl_list_for_each(other, &xframe->xshell->frames, link) {
			if (other != xframe) {
				next = other->frame;
				break;
			}
		}
		clock_follow(xframe->xshell, next);
	}
	listener_disconnect(&xframe->presented);
	listener_disconnect(&xframe->destroy);
	wl_list_remove(&xframe->link);
	free(xframe);
}

static void handle_new_frame(struct wl_listener *listener, void *data) {
	struct flame_xshell *xshell = wl_container_of(listener, xshell, new_output);
	struct flame_frame *frame = data;
	struct xshell_frame *xframe = calloc(1, sizeof(*xframe));
	if (!xframe) {
		return;
	}
	xframe->xshell = xshell;
	xframe->frame = frame;
	listener_connect(&xframe->presented, &frame->events.presented, handle_presented);
	listener_connect(&xframe->destroy, &frame->events.destroy, handle_frame_destroy);
	wl_list_insert(&xshell->frames, &xframe->link);
	if (!xshell->clock.frame) {
		clock_follow(xshell, frame);
	}
}

// the buffer node's owner side: a frame drew the window's picture
static void buffer_sampled(struct flame_buffer_node *node, struct flame_frame *frame,
		uint64_t draw_id, struct wlr_drm_syncobj_timeline *render_done, uint64_t render_point) {
	struct flame_xwindow *window = node->data;
	struct xpresent *xp = window->shown;
	if (!xp || node->key != xp) {
		return;
	}
	if (!xp->drawn_id) {
		xp->drawn_id = draw_id;
		xp->frame = frame;
	}
	if (render_done) {
		wlr_drm_syncobj_timeline_ref(render_done);
	}
	if (xp->render_done) {
		wlr_drm_syncobj_timeline_unref(xp->render_done);
	}
	xp->render_done = render_done;
	xp->render_point = render_point;
}

static const struct flame_buffer_node_impl buffer_impl = {
	.sampled = buffer_sampled,
};

// ---- X top-levels as flame views ----

static struct flame_xwindow *from_view(struct flame_view *view) {
	struct flame_xwindow *window = wl_container_of(view, window, view);
	return window;
}

static struct xs_window *x_of(struct flame_xwindow *window) {
	return xserver_window(window->xshell->xserver, window->id);
}

static uint32_t xview_configure(struct flame_view *view, int width, int height,
		bool fullscreen) {
	struct flame_xwindow *window = from_view(view);
	struct xs_window *x = x_of(window);
	if (x && width > 0 && height > 0 && (x->width != width || x->height != height)) {
		xserver_move_window(window->xshell->xserver, x, x->x, x->y, width, height);
	}
	window->width = width;
	window->height = height;
	// nothing to wait for: X has no acks, the app redraws when it gets to it
	return 0;
}

static bool xview_acked(struct flame_view *view, uint32_t serial) {
	return true;
}

static void xview_set_display_size(struct flame_view *view, int width, int height) {
}

static void xview_content_size(struct flame_view *view, int *width, int *height) {
	struct flame_xwindow *window = from_view(view);
	*width = window->width;
	*height = window->height;
}

static struct wlr_box xview_window_geometry(struct flame_view *view, bool pending) {
	return (struct wlr_box){0};
}

static void xview_set_activated(struct flame_view *view, bool activated) {
	struct flame_xwindow *window = from_view(view);
	struct flame_xserver *xserver = window->xshell->xserver;
	if (activated) {
		xserver_set_focus(xserver, window->id);
	} else if (xserver->focus == window->id) {
		xserver_set_focus(xserver, XS_FOCUS_NONE);
	}
}

static void xview_set_resizing(struct flame_view *view, bool resizing) {
}

static void xview_close(struct flame_view *view) {
	struct flame_xwindow *window = from_view(view);
	struct xs_window *x = x_of(window);
	if (x) {
		xserver_close_window(window->xshell->xserver, x);
	}
}

static struct wlr_surface *xview_surface(struct flame_view *view) {
	return NULL; // input goes through xshell
}

static const char *xview_title(struct flame_view *view) {
	static char title[256];
	struct flame_xwindow *window = from_view(view);
	struct xs_window *x = x_of(window);
	return x ? xs_window_title(window->xshell->xserver, x, title, sizeof(title)) : NULL;
}

// WM_CLASS is "instance\0class\0", the class is what bars call the app
static const char *xview_app_id(struct flame_view *view) {
	static char app_id[128];
	struct flame_xwindow *window = from_view(view);
	struct xs_window *x = x_of(window);
	struct xs_property *property = x ? xserver_property(window->xshell->xserver, x,
		"WM_CLASS") : NULL;
	if (!property || property->format != 8 || property->units == 0) {
		return NULL;
	}
	size_t len = property->units < sizeof(app_id) - 1 ? property->units : sizeof(app_id) - 1;
	memcpy(app_id, property->data, len);
	app_id[len] = '\0';
	size_t instance = strlen(app_id);
	return instance + 1 < len ? app_id + instance + 1 : app_id;
}

static void xview_placed(struct flame_view *view, struct wlr_box content) {
	struct flame_xwindow *window = from_view(view);
	struct xs_window *x = x_of(window);
	if (x) {
		xserver_move_window(window->xshell->xserver, x, content.x, content.y,
			content.width > 0 ? content.width : 1, content.height > 0 ? content.height : 1);
	}
	window->width = content.width;
	window->height = content.height;
}

static const struct flame_view_impl xview_impl = {
	.configure = xview_configure,
	.acked = xview_acked,
	.set_display_size = xview_set_display_size,
	.content_size = xview_content_size,
	.window_geometry = xview_window_geometry,
	.set_activated = xview_set_activated,
	.set_resizing = xview_set_resizing,
	.close = xview_close,
	.surface = xview_surface,
	.app_id = xview_app_id,
	.title = xview_title,
	.placed = xview_placed,
};

static bool is_xview(struct flame_view *view) {
	return view && view->impl == &xview_impl;
}

static struct flame_xwindow *window_create(struct flame_xshell *xshell, struct xs_window *x) {
	struct flame_server *server = xshell->server;
	struct flame_xwindow *window = calloc(1, sizeof(*window));
	if (!window) {
		return NULL;
	}
	window->xshell = xshell;
	window->id = x->id;
	window->floating = x->override_redirect;
	window->width = x->width;
	window->height = x->height;
	wl_list_init(&window->queue);
	wl_list_init(&window->retiring);
	if (window->floating) {
		// menus and tooltips: where the app put them, above everything
		window->float_tree = scene_tree_create(server->render.layers[FLAME_LAYER_OVERLAY]);
		window->content = window->float_tree
			? scene_buffer_create(window->float_tree, &buffer_impl, window) : NULL;
		if (window->float_tree) {
			scene_node_set_position(&window->float_tree->node, x->x, x->y);
		}
	} else {
		view_init(&window->view, &xview_impl, &server->desktop);
		window->content = scene_buffer_create(window->view.content, &buffer_impl, window);
	}
	if (!window->content) {
		if (window->float_tree) {
			scene_node_destroy(&window->float_tree->node);
		} else {
			view_finish(&window->view);
		}
		free(window);
		return NULL;
	}
	wl_list_insert(xshell->windows.prev, &window->link);
	if (!window->floating) {
		desktop_map_view(&server->desktop, &window->view);
	}
	wlr_log(WLR_DEBUG, "xshell: X window 0x%x %s", x->id,
		window->floating ? "floats" : "is a window");
	// drawn into before flame got to it: what's there already shows
	if (x->backing) {
		pixman_region32_t all;
		pixman_region32_init_rect(&all, 0, 0, (unsigned)pixman_image_get_width(x->backing),
			(unsigned)pixman_image_get_height(x->backing));
		xshell_damaged(xshell, x, &all);
		pixman_region32_fini(&all);
	}
	return window;
}

// ---- windows drawn with the cpu ----

// a top-level's backing as a wlr_buffer, for uploading it: the pixels stay
// the X server's, a ref keeps them alive while the buffer is
struct xcore {
	struct wlr_buffer base;
	pixman_image_t *image;
};

static void xcore_destroy(struct wlr_buffer *buffer) {
	struct xcore *core = wl_container_of(buffer, core, base);
	pixman_image_unref(core->image);
	free(core);
}

static bool xcore_begin(struct wlr_buffer *buffer, uint32_t flags, void **data,
		uint32_t *format, size_t *stride) {
	struct xcore *core = wl_container_of(buffer, core, base);
	*data = pixman_image_get_data(core->image);
	*format = pixman_image_get_format(core->image) == PIXMAN_a8r8g8b8
		? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888;
	*stride = (size_t)pixman_image_get_stride(core->image);
	return true;
}

static void xcore_end(struct wlr_buffer *buffer) {
}

static const struct wlr_buffer_impl xcore_impl = {
	.destroy = xcore_destroy,
	.begin_data_ptr_access = xcore_begin,
	.end_data_ptr_access = xcore_end,
};

static void core_drop(struct flame_xwindow *window) {
	if (window->core_texture) {
		wlr_texture_destroy(window->core_texture);
		window->core_texture = NULL;
	}
	if (window->core) {
		wlr_buffer_drop(&window->core->base);
		window->core = NULL;
	}
}

void xshell_damaged(struct flame_xshell *xshell, struct xs_window *top,
		const pixman_region32_t *region) {
	struct flame_xwindow *window = find_window(xshell, top->id);
	if (!window || window->shown || !top->backing) {
		return; // not shown here, or Present shows it
	}
	pixman_image_t *image = top->backing;
	struct wlr_renderer *renderer = xshell->server->render.renderer;
	bool fresh = !window->core || window->core->image != image || !window->core_texture;
	if (!fresh && !wlr_texture_update_from_buffer(window->core_texture, &window->core->base,
			region)) {
		fresh = true; // the renderer can't update in place: upload it all
	}
	if (fresh) {
		core_drop(window);
		struct xcore *core = calloc(1, sizeof(*core));
		if (!core) {
			return;
		}
		core->image = pixman_image_ref(image);
		wlr_buffer_init(&core->base, &xcore_impl, pixman_image_get_width(image),
			pixman_image_get_height(image));
		window->core = core;
		window->core_texture = wlr_texture_from_buffer(renderer, &core->base);
		if (!window->core_texture) {
			wlr_log(WLR_ERROR, "xshell: can't upload X window 0x%x", top->id);
			core_drop(window);
			return;
		}
	}
	// a new key every time: the frame repaints the window
	scene_node_set_position(&window->content->node, 0, 0);
	scene_buffer_set(window->content, window->core_texture,
		(const void *)++window->core_serial, pixman_image_get_width(image),
		pixman_image_get_height(image), pixman_image_get_format(image) != PIXMAN_a8r8g8b8,
		NULL, 0);
}

static void window_destroy(struct flame_xwindow *window) {
	struct flame_xshell *xshell = window->xshell;
	if (xshell->pointer_window == window->id) {
		xshell->pointer_window = 0;
	}
	// everything it was showing goes back to the app, nothing completes
	struct xpresent *shown = window->shown;
	window->shown = NULL;
	if (shown) {
		wl_list_init(&shown->link);
		wl_list_insert(&window->retiring, &shown->link);
	}
	wl_list_insert_list(&window->retiring, &window->queue);
	wl_list_init(&window->queue);
	struct xpresent *xp, *tmp;
	wl_list_for_each_safe(xp, tmp, &window->retiring, link) {
		if (!xp->released && xp->release) {
			wlr_drm_syncobj_timeline_signal(xp->release, xp->release_point);
		}
		xpresent_free(xp);
	}
	scene_buffer_set(window->content, NULL, NULL, 0, 0, false, NULL, 0);
	core_drop(window);
	if (window->floating) {
		scene_node_destroy(&window->float_tree->node);
	} else {
		desktop_unmap_view(&xshell->server->desktop, &window->view);
		view_finish(&window->view);
	}
	wl_list_remove(&window->link);
	free(window);
}

// X windows flame shows: mapped top-levels that can be drawn, from apps that
// don't draw over wayland already (steam's are bridged, see xbridge/)
static bool wanted(struct flame_xshell *xshell, struct xs_window *x) {
	return x->mapped && x->class != 2 && x->owner
		&& !xbridge_hides(&xshell->server->xbridge, x->owner->pid);
}

void xshell_windows_changed(struct flame_xshell *xshell) {
	struct xs_window *root = xshell->xserver->root;
	struct xs_window *x;
	wl_list_for_each(x, &root->children, link) {
		struct flame_xwindow *window = find_window(xshell, x->id);
		if (wanted(xshell, x)) {
			if (!window) {
				window_create(xshell, x);
			} else if (window->floating) {
				scene_node_set_position(&window->float_tree->node, x->x, x->y);
			}
		} else if (window) {
			window_destroy(window);
		}
	}
	desktop_notify(&xshell->server->desktop); // titles
}

void xshell_window_destroyed(struct flame_xshell *xshell, struct xs_window *x) {
	struct flame_xwindow *window = find_window(xshell, x->id);
	if (window) {
		window_destroy(window);
	}
}

// ---- input ----

static uint16_t x_state(struct flame_xshell *xshell) {
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(xshell->server->input.seat);
	// wlroots keeps modifiers in X's own bit order
	uint16_t state = keyboard ? (uint16_t)wlr_keyboard_get_modifiers(keyboard) : 0;
	return state | xshell->buttons;
}

static struct flame_xwindow *window_at(struct flame_xshell *xshell, double lx, double ly) {
	struct wlr_surface *surface;
	double sx, sy;
	struct flame_node *node = scene_node_at(&xshell->server->render.scene, lx, ly, &surface,
		&sx, &sy);
	if (!node || node->type != FLAME_NODE_BUFFER) {
		return NULL;
	}
	struct flame_buffer_node *buffer = wl_container_of(node, buffer, node);
	return buffer->impl == &buffer_impl ? buffer->data : NULL;
}

bool xshell_pointer_motion(struct flame_xshell *xshell, double lx, double ly, uint32_t time,
		struct flame_view **view) {
	struct flame_xserver *xserver = xshell->xserver;
	*view = NULL;
	// a held button keeps feeding the window it went down on
	struct flame_xwindow *window = xshell->buttons && xshell->pointer_window
		? find_window(xshell, xshell->pointer_window) : window_at(xshell, lx, ly);
	if (!window) {
		if (xshell->pointer_window) {
			xserver_pointer_leave(xserver, time);
			xshell->pointer_window = 0;
		}
		return false;
	}
	xshell->pointer_window = window->id;
	xserver_pointer_motion(xserver, x_of(window), (int)lx, (int)ly, x_state(xshell), time);
	*view = window->floating ? NULL : &window->view;
	return true;
}

static uint8_t x_button(uint32_t button) {
	switch (button) {
	case BTN_LEFT:
		return 1;
	case BTN_MIDDLE:
		return 2;
	case BTN_RIGHT:
		return 3;
	case BTN_SIDE:
		return 8;
	case BTN_EXTRA:
		return 9;
	default:
		return 0;
	}
}

bool xshell_pointer_button(struct flame_xshell *xshell, uint32_t button, bool pressed,
		uint32_t time) {
	if (!xshell->pointer_window) {
		return false;
	}
	uint8_t xbutton = x_button(button);
	if (!xbutton) {
		return true;
	}
	// the state an event carries is from before it
	uint16_t state = x_state(xshell);
	if (xbutton <= 5) {
		uint16_t bit = (uint16_t)(1u << (7 + xbutton));
		xshell->buttons = pressed ? xshell->buttons | bit : xshell->buttons & (uint16_t)~bit;
	}
	xserver_pointer_button(xshell->xserver, xbutton, pressed, state, time);
	return true;
}

bool xshell_pointer_axis(struct flame_xshell *xshell, bool vertical, double delta,
		uint32_t time) {
	if (!xshell->pointer_window) {
		return false;
	}
	// scrolling is buttons 4 to 7 on X, a click per wheel notch (15 units).
	// touchpads send small steps, they add up
	double *acc = &xshell->scroll[vertical ? 0 : 1];
	*acc += delta;
	uint16_t state = x_state(xshell);
	while (*acc <= -15 || *acc >= 15) {
		bool back = *acc < 0;
		uint8_t button = vertical ? (back ? 4 : 5) : (back ? 6 : 7);
		xserver_pointer_button(xshell->xserver, button, true, state, time);
		xserver_pointer_button(xshell->xserver, button, false, state, time);
		*acc += back ? 15 : -15;
	}
	return true;
}

bool xshell_key(struct flame_xshell *xshell, uint32_t keycode, bool pressed, uint32_t time) {
	if (!is_xview(xshell->server->desktop.focused)) {
		return false;
	}
	xserver_key(xshell->xserver, (uint8_t)(keycode + 8), pressed, x_state(xshell), time);
	return true;
}

int xshell_keysyms(struct flame_xshell *xshell, uint8_t keycode, uint32_t *out, int max) {
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(xshell->server->input.seat);
	if (!keyboard || !keyboard->keymap) {
		return 0;
	}
	int count = 0;
	for (int level = 0; level < max; level++) {
		const xkb_keysym_t *syms;
		int n = xkb_keymap_key_get_syms_by_level(keyboard->keymap, keycode, 0, level, &syms);
		out[level] = n > 0 ? syms[0] : 0;
		count += n > 0;
	}
	return count;
}

// ---- the gpu ----

int xshell_open_device(struct flame_xshell *xshell) {
	int drm_fd = wlr_renderer_get_drm_fd(xshell->server->render.renderer);
	if (drm_fd < 0) {
		return -1;
	}
	char *name = drmGetRenderDeviceNameFromFd(drm_fd);
	if (!name) {
		return -1;
	}
	int fd = open(name, O_RDWR | O_CLOEXEC);
	free(name);
	return fd;
}

size_t xshell_modifiers(struct flame_xshell *xshell, uint32_t format, uint64_t *out,
		size_t max) {
	const struct wlr_drm_format_set *formats = wlr_renderer_get_texture_formats(
		xshell->server->render.renderer, WLR_BUFFER_CAP_DMABUF);
	const struct wlr_drm_format *found = formats ? wlr_drm_format_set_get(formats, format) : NULL;
	size_t count = 0;
	for (size_t i = 0; found && i < found->len && count < max; i++) {
		out[count++] = found->modifiers[i];
	}
	return count;
}

// ---- setup ----

bool xshell_init(struct flame_xshell *xshell, struct flame_server *server,
		struct flame_xserver *xserver) {
	xshell->server = server;
	xshell->xserver = xserver;
	wl_list_init(&xshell->windows);
	wl_list_init(&xshell->frames);
	wl_list_init(&xshell->notifies);
	if (!timer_init(&xshell->msc_timer, server->loop, handle_msc_timer, xshell)) {
		return false;
	}
	listener_connect(&xshell->new_output, &server->outputs.events.new_frame,
		handle_new_frame);
	return true;
}

void xshell_finish(struct flame_xshell *xshell) {
	struct flame_xwindow *window, *tmp;
	wl_list_for_each_safe(window, tmp, &xshell->windows, link) {
		window_destroy(window);
	}
	struct xshell_frame *xframe, *ftmp;
	wl_list_for_each_safe(xframe, ftmp, &xshell->frames, link) {
		listener_disconnect(&xframe->presented);
		listener_disconnect(&xframe->destroy);
		wl_list_remove(&xframe->link);
		free(xframe);
	}
	struct xnotify *notify, *ntmp;
	wl_list_for_each_safe(notify, ntmp, &xshell->notifies, link) {
		wl_list_remove(&notify->link);
		free(notify);
	}
	timer_finish(&xshell->msc_timer);
	listener_disconnect(&xshell->new_output);
}
