#include "xshell/xshell.h"

#include <fcntl.h>
#include <math.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <drm_fourcc.h>
#include <time.h>
#include <unistd.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>
#include <xf86drm.h>
#include <xkbcommon/xkbcommon.h>

#include "anim/window.h"
#include "core/server.h"
#include "util/listener.h"
#include "util/timer.h"
#include "xbridge/xbridge.h"
#include "xshell/xsettings.h"
#include "xserver/xserver.h"

#define MODE_COPY 0
#define MODE_FLIP 1
#define MODE_SKIP 2
#define OPTION_ASYNC 1

// ---- client gpu buffers as textures ----

// a DRI3 pixmap's dmabuf as a wlr_buffer, the fds are its own copies
static bool grab_honored(const struct flame_xshell *xshell);
static pid_t view_pid(struct flame_xshell *xshell, struct flame_view *view);

// ---- X's coordinates and flame's ----

int xshell_to_x(const struct flame_xshell *xshell, double v) {
	return (int)lround(v * xshell->scale);
}

double xshell_from_x(const struct flame_xshell *xshell, double v) {
	return v / xshell->scale;
}

int xshell_pointer_to_x(const struct flame_xshell *xshell, double v) {
	return (int)floor(v * xshell->scale);
}

bool xshell_pointer_confine(const struct flame_xshell *xshell, double *x0, double *y0,
		double *x1, double *y1) {
	int x, y, width, height;
	if (!grab_honored(xshell)
			|| !xserver_pointer_confine(xshell->xserver, &x, &y, &width, &height)) {
		return false;
	}
	// the middle of its edge pixels: anywhere between floors into it
	*x0 = xshell_from_x(xshell, x + 0.5);
	*y0 = xshell_from_x(xshell, y + 0.5);
	*x1 = xshell_from_x(xshell, x + width - 0.5);
	*y1 = xshell_from_x(xshell, y + height - 0.5);
	return true;
}

// the middle of the X pixel the pointer's in, layout coords: who's under the
// pointer is decided there, so flame and X agree to the pixel. a pointer
// at 41.7 is in X's row 41, it can't be over a window that starts at 42
static double pointer_snap(const struct flame_xshell *xshell, double v) {
	return xshell_from_x(xshell, xshell_pointer_to_x(xshell, v) + 0.5);
}

// an X length in whole layout units
static int from_x(const struct flame_xshell *xshell, int v) {
	return (int)lround(xshell_from_x(xshell, v));
}

static struct xs_window *x_of(struct flame_xwindow *window) {
	return xserver_window(window->xshell->xserver, window->id);
}

struct xdmabuf {
	struct wlr_buffer base;
	struct wlr_dmabuf_attributes attribs;
};

static void xdmabuf_destroy(struct wlr_buffer *buffer) {
	struct xdmabuf *xdmabuf = wl_container_of(buffer, xdmabuf, base);
	// the renderer's import of it goes with it, every buffer must say so
	wlr_buffer_finish(buffer);
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
	uint32_t format; // drm fourcc and modifier, what copies of it are made as
	uint64_t modifier;
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
	xpixmap->format = pixmap->format;
	xpixmap->modifier = pixmap->modifier;
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

// an X window that presents, in its top-level's picture: the top-level
// itself, or a window in it with its own pictures (a browser's web view, a
// game's client window). shown where X has it, clipped to it and what it's
// in, stacked the way X stacks it. each has its own queue and pacing, like
// each has its own Present on Xorg
struct xlayer {
	struct wl_list link; // flame_xwindow.layers
	struct flame_xwindow *window;
	uint32_t id; // the X window presented to
	struct flame_buffer_node *node;
	int order; // where X paints it in the top-level, layers_update's

	struct xpresent *shown; // its picture right now
	struct wl_list queue; // xpresent.link, waiting for their msc, soonest first
	struct wl_list retiring; // xpresent.link, replaced, not done yet
	// the app draws into the buffer it just presented as soon as it has it
	// back (nvidia's GLX: one pixmap per window, presented every frame).
	// shown as it is, it would show half drawn: its presents are copied into
	// buffers of ours and it gets its own back right after, like Xorg and
	// Xwayland copy into a window. spare: copies nothing shows anymore
	bool copying;
	struct wl_list spare; // xcopy.link
	// how long the app takes from hearing a frame made it (CompleteNotify) to
	// its next present: quick to grow, slow to shrink. its completes are held
	// back to just in time for the render that can show the next one
	int64_t draw_time;
	int64_t woken; // when the last complete went out, while drawing
	bool drawing;
};

struct xpresent {
	struct wl_list link; // xlayer.queue or .retiring
	struct xlayer *layer;
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

	// completed: its complete went out. released: nothing of ours reads it
	// anymore. given_back: the app has the buffer back (release point,
	// IdleNotify), for a copy that's as soon as the copy is done
	bool completed, released, given_back;
	// the app gets it back once its acquire point went off and every older
	// present of the same pixmap went back: one timeline per pixmap for both
	// is nvidia's way, a release ahead of a pending acquire says that frame
	// is drawn when it isn't
	struct wlr_drm_syncobj_timeline_waiter back_waiter;
	bool back_waiting, back_wanted;
	// copying layers: what's shown instead of the app's buffer, done on the
	// gpu at copy_point of the copy timeline
	struct xcopy *copy;
	uint64_t copy_point;
	struct wlr_drm_syncobj_timeline_waiter copy_waiter;
	bool copy_waiting, copied;
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

// a buffer of ours a present got copied into, and its texture
struct xcopy {
	struct wl_list link; // xlayer.spare
	struct wlr_buffer *buffer;
	struct wlr_texture *texture;
	int width, height;
	bool alpha;
};

static void copy_destroy(struct xcopy *copy) {
	wl_list_remove(&copy->link);
	wlr_texture_destroy(copy->texture);
	wlr_buffer_drop(copy->buffer);
	free(copy);
}

#define SPARE_COPIES 3

// back to the layer for the next present, a few at most
static void copy_put(struct xlayer *layer, struct xcopy *copy) {
	wl_list_insert(&layer->spare, &copy->link);
	int count = 0;
	struct xcopy *each, *tmp;
	wl_list_for_each_safe(each, tmp, &layer->spare, link) {
		if (++count > SPARE_COPIES) {
			copy_destroy(each);
		}
	}
}

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
	if (xp->back_waiting) {
		wlr_drm_syncobj_timeline_waiter_finish(&xp->back_waiter);
	}
	if (xp->copy_waiting) {
		wlr_drm_syncobj_timeline_waiter_finish(&xp->copy_waiter);
	}
	if (xp->copy) {
		copy_put(xp->layer, xp->copy);
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
	if (xp->completed && xp->released && xp->given_back && xp != xp->layer->shown) {
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
	struct flame_xshell *xshell = xp->layer->window->xshell;
	xserver_present_complete(xshell->xserver, xp->layer->id, xp->serial, false, mode,
		(uint64_t)(when / 1000), msc);
	maybe_free(xp);
}

// a held complete goes out: the app hears its frame made it, when and on which
// vblank, and starts the next one. woke: it was due (the app waits on it and
// its draw time is measured from now), not sent early because the app went on
static void deliver(struct xpresent *xp, bool woke) {
	struct xlayer *layer = xp->layer;
	xp->held = false;
	if (woke) {
		layer->woken = now_ns();
		layer->drawing = true;
	}
	// its own buffer on screen is a flip, a copy of it a copy
	complete(xp, xp->copied ? MODE_COPY : MODE_FLIP, xp->done_when, xp->done_msc);
}

// the app presented again without waiting for its complete: no point holding
// what's left, it goes out now
static void deliver_held(struct xlayer *layer) {
	struct xpresent *xp, *tmp;
	if (layer->shown && layer->shown->held) {
		deliver(layer->shown, false);
	}
	wl_list_for_each_safe(xp, tmp, &layer->retiring, link) {
		if (xp->held) {
			deliver(xp, false);
		}
	}
}

static void give_back(struct xpresent *xp);

static void handle_back_ready(struct wlr_drm_syncobj_timeline_waiter *waiter) {
	struct xpresent *xp = wl_container_of(waiter, xp, back_waiter);
	wlr_drm_syncobj_timeline_waiter_finish(&xp->back_waiter);
	xp->back_waiting = false;
	give_back(xp);
}

// serials wrap, older is behind
static bool older(uint32_t a, uint32_t b) {
	return (int32_t)(a - b) < 0;
}

// an older present of the same pixmap the app doesn't have back yet
static bool older_out(struct xpresent *xp) {
	struct xlayer *layer = xp->layer;
	struct xpresent *other;
	wl_list_for_each(other, &layer->retiring, link) {
		if (other != xp && other->pixmap == xp->pixmap && !other->given_back
				&& older(other->serial, xp->serial)) {
			return true;
		}
	}
	return false;
}

// the app can have its buffer back: its release point goes off (explicit
// sync), its idle fence and IdleNotify (implicit). in order, see back_waiter
static void give_back(struct xpresent *xp) {
	if (xp->given_back || xp->back_waiting) {
		return;
	}
	bool done = true;
	if (xp->acquire && wlr_drm_syncobj_timeline_check(xp->acquire, xp->acquire_point, 0,
			&done) && !done) {
		struct flame_xshell *xshell = xp->layer->window->xshell;
		if (wlr_drm_syncobj_timeline_waiter_init(&xp->back_waiter, xp->acquire,
				xp->acquire_point, 0, xshell->server->loop, handle_back_ready)) {
			xp->back_waiting = true;
			return;
		}
	}
	if (older_out(xp)) {
		xp->back_wanted = true; // the older one going back sends this one
		return;
	}
	xp->given_back = true;
	xp->back_wanted = false;
	if (xp->release) {
		wlr_drm_syncobj_timeline_signal(xp->release, xp->release_point);
	}
	struct xlayer *layer = xp->layer;
	struct flame_xshell *xshell = layer->window->xshell;
	xserver_present_idle(xshell->xserver, layer->id, xp->serial, xp->pixmap_id,
		xp->idle_fence);
	// the oldest of the same pixmap that waited on this one goes next, it
	// sends the one after it
	struct xpresent *next = NULL, *other;
	wl_list_for_each(other, &layer->retiring, link) {
		if (other->back_wanted && other->pixmap == xp->pixmap
				&& (!next || older(other->serial, next->serial))) {
			next = other;
		}
	}
	other = layer->shown;
	if (other && other->back_wanted && other->pixmap == xp->pixmap
			&& (!next || older(other->serial, next->serial))) {
		next = other;
	}
	maybe_free(xp);
	if (next) {
		give_back(next);
	}
}

// nothing of ours reads it anymore: a copy goes back to the layer, the app's
// own buffer goes back to the app
static void released(struct xpresent *xp) {
	xp->released = true;
	if (xp->copy) {
		copy_put(xp->layer, xp->copy);
		xp->copy = NULL;
		maybe_free(xp);
		return;
	}
	if (xp->given_back) {
		maybe_free(xp);
	} else {
		give_back(xp); // frees it once it's back
	}
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

static void handle_copied(struct wlr_drm_syncobj_timeline_waiter *waiter) {
	struct xpresent *xp = wl_container_of(waiter, xp, copy_waiter);
	wlr_drm_syncobj_timeline_waiter_finish(&xp->copy_waiter);
	xp->copy_waiting = false;
	give_back(xp);
}

static struct xcopy *copy_take(struct xlayer *layer, const struct xpixmap *pixmap) {
	struct xcopy *copy, *tmp;
	wl_list_for_each_safe(copy, tmp, &layer->spare, link) {
		if (copy->width == pixmap->width && copy->height == pixmap->height
				&& copy->alpha == !pixmap->opaque) {
			wl_list_remove(&copy->link);
			wl_list_init(&copy->link);
			return copy;
		}
		copy_destroy(copy); // the window changed size
	}
	struct flame_render *render = &layer->window->xshell->server->render;
	copy = calloc(1, sizeof(*copy));
	if (!copy) {
		return NULL;
	}
	// the app's own format and layout: it draws into those on this gpu, so
	// we can too. failing that whatever layout the allocator likes
	uint64_t modifiers[] = {pixmap->modifier, DRM_FORMAT_MOD_INVALID};
	for (size_t i = 0; i < 2 && !copy->buffer; i++) {
		struct wlr_drm_format format = {
			.format = pixmap->format,
			.len = 1,
			.capacity = 1,
			.modifiers = &modifiers[i],
		};
		copy->buffer = wlr_allocator_create_buffer(render->allocator, pixmap->width,
			pixmap->height, &format);
	}
	copy->texture = copy->buffer ? wlr_texture_from_buffer(render->renderer, copy->buffer)
		: NULL;
	if (!copy->texture) {
		if (copy->buffer) {
			wlr_buffer_drop(copy->buffer);
		}
		free(copy);
		return NULL;
	}
	copy->width = pixmap->width;
	copy->height = pixmap->height;
	copy->alpha = !pixmap->opaque;
	wl_list_init(&copy->link);
	return copy;
}

// the present's pixels into a buffer of ours on the gpu, once its acquire
// point goes off. the app gets its buffer back when the copy's done. false:
// no copy, it shows as it is
static bool copy_frame(struct xpresent *xp) {
	struct xlayer *layer = xp->layer;
	struct flame_xshell *xshell = layer->window->xshell;
	struct wlr_renderer *renderer = xshell->server->render.renderer;
	struct xcopy *copy = copy_take(layer, xp->pixmap);
	if (!copy) {
		return false;
	}
	if (!xshell->copy_timeline && renderer->features.timeline) {
		int drm_fd = wlr_renderer_get_drm_fd(renderer);
		xshell->copy_timeline = drm_fd >= 0 ? wlr_drm_syncobj_timeline_create(drm_fd) : NULL;
	}
	struct wlr_drm_syncobj_timeline *timeline = xshell->copy_timeline;
	uint64_t point = ++xshell->copy_point;
	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(renderer, copy->buffer,
		&(struct wlr_buffer_pass_options){
			.signal_timeline = timeline,
			.signal_point = point,
		});
	if (pass) {
		wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
			.texture = xp->pixmap->texture,
			.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
			.wait_timeline = timeline ? xp->acquire : NULL,
			.wait_point = xp->acquire_point,
		});
	}
	if (!pass || !wlr_render_pass_submit(pass)) {
		copy_put(layer, copy);
		return false;
	}
	xp->copy = copy;
	xp->copied = true;
	xp->copy_point = timeline ? point : 0;
	if (timeline && wlr_drm_syncobj_timeline_waiter_init(&xp->copy_waiter, timeline, point, 0,
			xshell->server->loop, handle_copied)) {
		xp->copy_waiting = true;
	} else {
		give_back(xp); // implicit sync: our reads are ordered before theirs
	}
	return true;
}

// the layer has a present of this pixmap the app doesn't have back
static bool holds(struct xlayer *layer, const struct xpixmap *pixmap) {
	struct xpresent *xp;
	if (layer->shown && layer->shown->pixmap == pixmap && !layer->shown->given_back) {
		return true;
	}
	wl_list_for_each(xp, &layer->queue, link) {
		if (xp->pixmap == pixmap) {
			return true;
		}
	}
	wl_list_for_each(xp, &layer->retiring, link) {
		if (xp->pixmap == pixmap && !xp->given_back) {
			return true;
		}
	}
	return false;
}

static void retire(struct xlayer *layer, struct xpresent *xp) {
	struct flame_xshell *xshell = layer->window->xshell;
	stop_acquiring(xp);
	wl_list_insert(&layer->retiring, &xp->link);
	if (!xp->drawn_id) {
		// never made it to the screen. the buffer goes back now, the complete
		// waits for its msc like on Xorg: apps (NVIDIA's) throttle on how
		// many are still out, a skip right away has them render the next one
		xp->skipped = true;
		uint64_t now_msc = msc_at(xshell, now_ns());
		if (xp->target <= now_msc) {
			complete(xp, MODE_SKIP, msc_time(xshell, now_msc), now_msc);
		}
	}
	release_when_idle(xshell, xp);
}

// the picture where it goes in the view: X's window starts left and top of
// what flame shows by its shadow
static void place_content(struct flame_xwindow *window) {
	int x = window->floating ? 0 : -window->ext_left;
	int y = window->floating ? 0 : -window->ext_top;
	scene_node_set_position(&window->content->node, from_x(window->xshell, x),
		from_x(window->xshell, y));
}

// a picture for a buffer node, a frame only when something about it changed
static void buffer_show(struct flame_buffer_node *node, struct wlr_texture *texture,
		const void *key, int width, int height, bool opaque,
		struct wlr_drm_syncobj_timeline *timeline, uint64_t point) {
	if (node->texture != texture || node->key != key || node->width != width
			|| node->height != height || node->opaque != opaque
			|| node->wait_timeline != timeline || node->wait_point != point) {
		scene_buffer_set(node, texture, key, width, height, opaque, timeline, point);
	}
}

// win is in top and it and everything between them is mapped
static bool x_viewable_in(const struct xs_window *top, const struct xs_window *win) {
	int depth = 0;
	for (const struct xs_window *w = win; w != top; w = w->parent) {
		if (!w || !w->parent || !w->mapped || ++depth > 64) {
			return false;
		}
	}
	return true;
}

// where X has win in top: its origin there and the part of it that shows,
// cut by everything it's in. false when it isn't viewable in top, or none
// of it shows
static bool x_visible(const struct xs_window *top, const struct xs_window *win, int *ox,
		int *oy, struct wlr_box *clip) {
	if (!x_viewable_in(top, win)) {
		return false;
	}
	const struct xs_window *chain[64];
	int len = 0;
	for (const struct xs_window *w = win; w != top; w = w->parent) {
		chain[len++] = w;
	}
	int x = 0, y = 0;
	struct wlr_box box = {0, 0, top->width, top->height};
	for (int i = len - 1; i >= 0; i--) {
		x += chain[i]->x + chain[i]->border;
		y += chain[i]->y + chain[i]->border;
		struct wlr_box inside = {x, y, chain[i]->width, chain[i]->height}, cut;
		if (!wlr_box_intersection(&cut, &box, &inside)) {
			return false;
		}
		box = cut;
	}
	*ox = x;
	*oy = y;
	*clip = box;
	return true;
}

static void core_update(struct flame_xwindow *window);

// what a layer shows goes, the app gets its buffer back: X lost the window's
// contents. it shows nothing until it draws again
static void layer_drop(struct xlayer *layer) {
	struct xpresent *xp = layer->shown;
	if (!xp) {
		return;
	}
	layer->shown = NULL;
	scene_node_set_enabled(&layer->node->node, false);
	scene_buffer_set(layer->node, NULL, NULL, 0, 0, false, NULL, 0);
	retire(layer, xp);
	core_update(layer->window);
}

// a layer's picture where X has its window, cut to what of that window
// shows: Present puts the pixmap at the window's corner plus its offset and
// nothing outside the window. off while its window isn't viewable
static void layer_place(struct xlayer *layer) {
	struct flame_xwindow *window = layer->window;
	struct flame_xshell *xshell = window->xshell;
	struct xpresent *xp = layer->shown;
	struct xs_window *top = x_of(window);
	struct xs_window *x = xserver_window(xshell->xserver, layer->id);
	if (xp && top && x && !x_viewable_in(top, x)) {
		// unmapped: what it showed is gone, like X loses a window's contents.
		// mapped again it shows nothing until it draws, never the old picture
		// (a menu flashing what it showed last time)
		layer_drop(layer);
		return;
	}
	int ox, oy;
	struct wlr_box clip, shows;
	if (!xp || !top || !x || !x_visible(top, x, &ox, &oy, &clip)) {
		scene_node_set_enabled(&layer->node->node, false);
		return;
	}
	struct wlr_box picture = {ox + xp->x_off, oy + xp->y_off, xp->pixmap->width,
		xp->pixmap->height};
	if (!wlr_box_intersection(&shows, &picture, &clip)) {
		scene_node_set_enabled(&layer->node->node, false);
		return;
	}
	scene_node_set_position(&layer->node->node, from_x(xshell, shows.x),
		from_x(xshell, shows.y));
	struct wlr_fbox src = {0};
	if (!wlr_box_equal(&shows, &picture)) {
		src = (struct wlr_fbox){shows.x - picture.x, shows.y - picture.y, shows.width,
			shows.height};
	}
	struct wlr_fbox *had = &layer->node->src;
	if (had->x != src.x || had->y != src.y || had->width != src.width
			|| had->height != src.height) {
		scene_buffer_set_source(layer->node, wlr_fbox_empty(&src) ? NULL : &src);
	}
	// a copy is read once the copy's done, the app's buffer once it's drawn
	struct wlr_texture *texture = xp->copy ? xp->copy->texture : xp->pixmap->texture;
	struct wlr_drm_syncobj_timeline *wait = xp->copy
		? (xp->copy_point ? xshell->copy_timeline : NULL) : xp->acquire;
	uint64_t wait_point = xp->copy ? xp->copy_point : xp->acquire_point;
	buffer_show(layer->node, texture, xp, from_x(xshell, shows.width),
		from_x(xshell, shows.height), xp->pixmap->opaque, wait, wait_point);
	scene_node_set_enabled(&layer->node->node, true);
}

// the top-level presents to itself: that's its picture, not the cpu's
static bool self_presented(const struct flame_xwindow *window) {
	struct xlayer *layer;
	wl_list_for_each(layer, &window->layers, link) {
		if (layer->id == window->id && layer->shown) {
			return true;
		}
	}
	return false;
}

// the cpu picture's node is the window's input too, where X has the window:
// until something's drawn, or while the top-level presents to itself, it's
// an empty one at X's size. input goes by where X has it, drawn or not
static void core_input(struct flame_xwindow *window) {
	struct xs_window *x = x_of(window);
	if (!x || (window->core_texture && !self_presented(window))) {
		return;
	}
	buffer_show(window->core_node, NULL, NULL, from_x(window->xshell, x->width),
		from_x(window->xshell, x->height), false, NULL, 0);
}

// the top-level presenting to itself: that's its picture, the cpu's goes
static void core_update(struct flame_xwindow *window) {
	if (self_presented(window)) {
		core_input(window);
	}
}

static void show(struct xlayer *layer, struct xpresent *xp) {
	struct xpresent *old = layer->shown;
	layer->shown = xp;
	wl_list_init(&xp->link);
	if (layer->copying && !xp->copied) {
		copy_frame(xp);
	}
	layer_place(layer);
	if (old) {
		wl_list_remove(&old->link);
		retire(layer, old);
	} else {
		wlr_log(WLR_DEBUG, "xshell: 0x%x in 0x%x shows a picture at %d,%d", layer->id,
			layer->window->id, layer->node->node.x, layer->node->node.y);
		core_update(layer->window);
	}
}

// waits for the refresh before its msc. one already queued for the same msc
// never makes it, like on Xorg
static void enqueue(struct xlayer *layer, struct xpresent *xp) {
	struct xpresent *queued, *tmp;
	wl_list_for_each_safe(queued, tmp, &layer->queue, link) {
		if (queued->target == xp->target) {
			wl_list_remove(&queued->link);
			retire(layer, queued);
		}
	}
	struct wl_list *before = &layer->queue;
	wl_list_for_each(queued, &layer->queue, link) {
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
	struct xlayer *layer;
	wl_list_for_each(window, &xshell->windows, link) wl_list_for_each(layer, &window->layers, link) {
		struct xpresent *shown = layer->shown;
		if (shown && shown->held) {
			next = !next || shown->held_until < next ? shown->held_until : next;
		}
		if (shown && !shown->completed && !shown->drawn_id && shown->target - now_msc < MSC_FAR) {
			int64_t at = msc_time(xshell, shown->target + 1);
			next = !next || at < next ? at : next;
		}
		struct xpresent *xp;
		wl_list_for_each(xp, &layer->retiring, link) {
			if (xp->held) {
				next = !next || xp->held_until < next ? xp->held_until : next;
			}
			if (xp->skipped && !xp->completed && xp->target - now_msc < MSC_FAR) {
				int64_t at = msc_time(xshell, xp->target);
				next = !next || at < next ? at : next;
			}
		}
		if (wl_list_empty(&layer->queue)) {
			continue;
		}
		struct xpresent *first = wl_container_of(layer->queue.next, first, link);
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
	struct xlayer *layer;
	wl_list_for_each(window, &xshell->windows, link) wl_list_for_each(layer, &window->layers, link) {
		struct xpresent *xp, *tmp;
		if (layer->shown && layer->shown->held && layer->shown->held_until <= now) {
			deliver(layer->shown, true);
		}
		wl_list_for_each_safe(xp, tmp, &layer->retiring, link) {
			if (xp->held && xp->held_until <= now) {
				deliver(xp, true);
			}
		}
		while (!wl_list_empty(&layer->queue)) {
			struct xpresent *first = wl_container_of(layer->queue.next, first, link);
			if (first->acquiring || first->target > now_msc + 1) {
				break; // the ones behind it wait too, order stays
			}
			wl_list_remove(&first->link);
			show(layer, first);
		}
		// a refresh past its msc and nothing drew it: the window is covered or
		// on another workspace. it completes anyway like a copy on Xorg, or a
		// FIFO app waits on it forever
		struct xpresent *shown = layer->shown;
		if (shown && !shown->completed && !shown->drawn_id && shown->target < now_msc) {
			complete(shown, shown->copied ? MODE_COPY : MODE_FLIP, msc_time(xshell, now_msc),
				now_msc);
		}
		wl_list_for_each_safe(xp, tmp, &layer->retiring, link) {
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
	process_msc(xp->layer->window->xshell, now_ns());
}

// the app's gpu work for it is done. if not, a wait starts: false until then
static bool acquired(struct xpresent *xp) {
	bool done = false;
	if (!wlr_drm_syncobj_timeline_check(xp->acquire, xp->acquire_point,
			DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, &done) || done) {
		return true; // can't tell, or done: show it
	}
	xp->acquiring = wlr_drm_syncobj_timeline_waiter_init(&xp->acquire_waiter, xp->acquire,
		xp->acquire_point, 0, xp->layer->window->xshell->server->loop, handle_acquired);
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

// ---- layers ----

static const struct flame_buffer_node_impl layer_impl;

// where each layer's window is in X's painting order in its top-level:
// parents before what's in them, siblings bottom to top
static void paint_order(struct flame_xwindow *window, struct xs_window *x, int *at) {
	int here = (*at)++;
	struct xlayer *layer;
	wl_list_for_each(layer, &window->layers, link) {
		if (layer->id == x->id) {
			layer->order = here;
		}
	}
	struct xs_window *child;
	wl_list_for_each(child, &x->children, link) {
		paint_order(window, child, at);
	}
}

// the layers stacked the way X paints their windows, over the cpu picture
static void layers_order(struct flame_xwindow *window) {
	struct xs_window *top = x_of(window);
	if (!top || wl_list_empty(&window->layers)) {
		return;
	}
	int at = 0;
	paint_order(window, top, &at);
	// a few at most, insertion sort
	struct wl_list sorted;
	wl_list_init(&sorted);
	while (!wl_list_empty(&window->layers)) {
		struct xlayer *layer = wl_container_of(window->layers.next, layer, link), *other;
		wl_list_remove(&layer->link);
		struct wl_list *after = &sorted;
		wl_list_for_each(other, &sorted, link) {
			if (other->order > layer->order) {
				break;
			}
			after = &other->link;
		}
		wl_list_insert(after, &layer->link);
	}
	wl_list_insert_list(&window->layers, &sorted);
	// raising only when it's out of order: every raise is a frame
	struct wl_list *node = window->content->children.next;
	bool in_order = node == &window->core_node->node.link;
	struct xlayer *layer;
	wl_list_for_each(layer, &window->layers, link) {
		node = node->next;
		in_order &= node == &layer->node->node.link;
	}
	if (!in_order) {
		scene_node_lower_to_bottom(&window->core_node->node);
		wl_list_for_each(layer, &window->layers, link) {
			scene_node_raise_to_top(&layer->node->node);
		}
	}
}

static struct xlayer *layer_get(struct flame_xwindow *window, uint32_t id) {
	struct xlayer *layer;
	wl_list_for_each(layer, &window->layers, link) {
		if (layer->id == id) {
			return layer;
		}
	}
	layer = calloc(1, sizeof(*layer));
	if (!layer) {
		return NULL;
	}
	layer->node = scene_buffer_create(window->content, &layer_impl, layer);
	if (!layer->node) {
		free(layer);
		return NULL;
	}
	scene_node_set_enabled(&layer->node->node, false);
	layer->window = window;
	layer->id = id;
	wl_list_init(&layer->queue);
	wl_list_init(&layer->retiring);
	wl_list_init(&layer->spare);
	wl_list_insert(window->layers.prev, &layer->link);
	layers_order(window);
	return layer;
}

// everything it was showing goes back to the app, nothing completes
static void layer_destroy(struct xlayer *layer) {
	struct xpresent *shown = layer->shown;
	layer->shown = NULL;
	if (shown) {
		wl_list_init(&shown->link);
		wl_list_insert(&layer->retiring, &shown->link);
	}
	wl_list_insert_list(&layer->retiring, &layer->queue);
	wl_list_init(&layer->queue);
	struct xpresent *xp, *tmp;
	wl_list_for_each_safe(xp, tmp, &layer->retiring, link) {
		if (!xp->given_back && xp->release) {
			wlr_drm_syncobj_timeline_signal(xp->release, xp->release_point);
		}
		xpresent_free(xp);
	}
	struct xcopy *copy, *ctmp;
	wl_list_for_each_safe(copy, ctmp, &layer->spare, link) {
		copy_destroy(copy);
	}
	scene_node_destroy(&layer->node->node);
	wl_list_remove(&layer->link);
	struct flame_xwindow *window = layer->window;
	free(layer);
	core_update(window);
}

// moved into another top-level (an app reparenting its web view): the
// layer goes with it, what it shows and all
static void layer_move(struct xlayer *layer, struct flame_xwindow *to) {
	struct flame_xwindow *from = layer->window;
	wlr_log(WLR_DEBUG, "xshell: 0x%x goes from 0x%x to 0x%x%s", layer->id, from->id, to->id,
		layer->shown ? ", its picture with it" : "");
	wl_list_remove(&layer->link);
	wl_list_insert(to->layers.prev, &layer->link);
	layer->window = to;
	scene_node_reparent(&layer->node->node, to->content);
	core_update(from);
	core_update(to);
	layers_order(to);
}

// the X tree changed: every layer where its window is now
static void layers_update(struct flame_xshell *xshell) {
	struct flame_xwindow *window;
	wl_list_for_each(window, &xshell->windows, link) {
		struct xlayer *layer, *tmp;
		wl_list_for_each_safe(layer, tmp, &window->layers, link) {
			struct flame_xwindow *now = window_of(xshell, layer->id);
			if (!now) {
				layer_destroy(layer);
			} else if (now != window) {
				layer_move(layer, now);
			}
		}
	}
	wl_list_for_each(window, &xshell->windows, link) {
		layers_order(window);
		struct xlayer *layer;
		wl_list_for_each(layer, &window->layers, link) {
			layer_place(layer);
		}
	}
}

void xshell_present(struct flame_xshell *xshell, const struct xs_present *present) {
	struct flame_xwindow *window = window_of(xshell, present->window);
	if (!window) {
		// mapped in the same breath, the window shows up first
		xshell_windows_changed(xshell);
		window = window_of(xshell, present->window);
	}
	struct xlayer *layer = window ? layer_get(window, present->window) : NULL;
	if (layer) {
		// woken by its complete: that's its draw time. an app idle for a while
		// (over 50ms) isn't drawing frame after frame, that's no sample
		if (layer->drawing) {
			int64_t took = now_ns() - layer->woken;
			if (took < 50000000LL) {
				layer->draw_time = took > layer->draw_time
					? took : layer->draw_time + (took - layer->draw_time) / 8;
			}
			layer->drawing = false;
		}
		deliver_held(layer);
	}
	struct xpixmap *xpixmap = xpixmap_get(xshell, present->pixmap);
	struct wlr_drm_syncobj_timeline *acquire = timeline_of(xshell, present->acquire);
	struct wlr_drm_syncobj_timeline *release = timeline_of(xshell, present->release);
	if (!layer || !xpixmap || (present->acquire && !acquire)) {
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
	if (!layer->copying && holds(layer, xpixmap)) {
		// a pixmap of ours presented again: the app draws into what it
		// presented as soon as it's back, see xlayer.copying. what shows
		// now becomes a copy so the app can have its buffer
		wlr_log(WLR_DEBUG, "xshell: X window 0x%x presents one buffer over and over, copying",
			layer->id);
		layer->copying = true;
		struct xpresent *shown = layer->shown;
		if (shown && !shown->copied && !shown->given_back && copy_frame(shown)) {
			layer_place(layer);
		}
	}
	struct xpresent *xp = calloc(1, sizeof(*xp));
	if (!xp) {
		return;
	}
	xp->layer = layer;
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
		enqueue(layer, xp);
	} else if (xp->target <= now_msc + 1 && wl_list_empty(&layer->queue)) {
		show(layer, xp);
	} else {
		enqueue(layer, xp);
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
static int64_t app_wake(const struct xlayer *layer, int64_t render_at) {
	if (!render_at || layer->draw_time <= 0) {
		return 0;
	}
	int64_t margin = layer->draw_time / 2 > JIT_MARGIN_NS ? layer->draw_time / 2 : JIT_MARGIN_NS;
	return render_at - layer->draw_time - margin;
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
	int64_t wake = app_wake(xp->layer, schedule_render_at(&frame->schedule, now));
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
	struct xlayer *layer;
	wl_list_for_each(window, &xshell->windows, link) wl_list_for_each(layer, &window->layers, link) {
		struct xpresent *xp, *tmp;
		wl_list_for_each_safe(xp, tmp, &layer->retiring, link) {
			check_presented(xframe->xshell, xp, xframe->frame, event);
		}
		if (layer->shown) {
			check_presented(xframe->xshell, layer->shown, xframe->frame, event);
		}
	}
	// the msc moved: queued presents and NotifyMSCs may be due
	process_msc(xshell, now_ns());
}

static void handle_frame_destroy(struct wl_listener *listener, void *data) {
	struct xshell_frame *xframe = wl_container_of(listener, xframe, destroy);
	// whatever was waiting on that output won't hear from it again
	struct flame_xwindow *window;
	struct xlayer *layer;
	wl_list_for_each(window, &xframe->xshell->windows, link) wl_list_for_each(layer, &window->layers, link) {
		struct xpresent *xp, *tmp;
		wl_list_for_each_safe(xp, tmp, &layer->retiring, link) {
			if (xp->frame == xframe->frame) {
				xp->frame = NULL;
				if (xp->held) {
					deliver(xp, false); // it made it, just not told yet
				} else {
					complete(xp, MODE_SKIP, now_ns(), msc_at(xframe->xshell, now_ns()));
				}
			}
		}
		struct xpresent *shown = layer->shown;
		if (shown && shown->held && shown->frame == xframe->frame) {
			deliver(shown, false);
		}
		if (shown && shown->frame == xframe->frame) {
			shown->frame = NULL;
			shown->drawn_id = 0; // drawn again on another output
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

// the buffer node's owner side: a frame drew a layer's picture
static void layer_sampled(struct flame_buffer_node *node, struct flame_frame *frame,
		uint64_t draw_id, struct wlr_drm_syncobj_timeline *render_done, uint64_t render_point) {
	struct xlayer *layer = node->data;
	struct xpresent *xp = layer->shown;
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

// what the window cut out of its input shape (a toolkit's shadow) goes to
// whatever is below it. x, y: in the node, which is at dx, dy in the window
static bool accepts_input(struct flame_xwindow *window, int dx, int dy, double x, double y) {
	struct xs_window *xw = xserver_window(window->xshell->xserver, window->id);
	return !xw || xserver_window_accepts_input(xw, xshell_to_x(window->xshell, x + dx),
		xshell_to_x(window->xshell, y + dy));
}

static bool layer_accepts_input(struct flame_buffer_node *node, double x, double y) {
	struct xlayer *layer = node->data;
	return accepts_input(layer->window, node->node.x, node->node.y, x, y);
}

static bool core_accepts_input(struct flame_buffer_node *node, double x, double y) {
	return accepts_input(node->data, 0, 0, x, y);
}

static const struct flame_buffer_node_impl layer_impl = {
	.sampled = layer_sampled,
	.accepts_input = layer_accepts_input,
};

static const struct flame_buffer_node_impl core_impl = {
	.accepts_input = core_accepts_input,
};

// ---- X top-levels as flame views ----

static void x_stack_update(struct flame_xshell *xshell);

static struct flame_xwindow *from_view(struct flame_view *view) {
	struct flame_xwindow *window = wl_container_of(view, window, view);
	return window;
}

// what flame shows of it (inside the shadow) to X's size and back
static int x_width(const struct flame_xwindow *window, int width) {
	return width + window->ext_left + window->ext_right;
}

static int x_height(const struct flame_xwindow *window, int height) {
	return height + window->ext_top + window->ext_bottom;
}

static uint32_t xview_configure(struct flame_view *view, int width, int height,
		bool fullscreen) {
	struct flame_xwindow *window = from_view(view);
	struct xs_window *x = x_of(window);
	width = x_width(window, xshell_to_x(window->xshell, width));
	height = x_height(window, xshell_to_x(window->xshell, height));
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
	*width = from_x(window->xshell, window->width - window->ext_left - window->ext_right);
	*height = from_x(window->xshell, window->height - window->ext_top - window->ext_bottom);
}

static struct wlr_box xview_window_geometry(struct flame_view *view, bool pending) {
	return (struct wlr_box){0};
}

static bool is_xview(struct flame_view *view);

// the app (its process) an X window is from, 0 for none
static pid_t x_pid(struct flame_xserver *xserver, uint32_t id) {
	struct xs_window *x = id > XS_FOCUS_POINTER_ROOT ? xserver_window(xserver, id) : NULL;
	return x && x->owner ? x->owner->pid : 0;
}

// flame's focus went from an X app to something else: X's focus leaves it
// once this round of focus changes is done, wherever in the app it is (a
// menu it opened and focused is a top-level of its own). another X window
// taking it takes it straight over, apps don't see a detour through None
static void handle_unfocus_idle(void *data) {
	struct flame_xshell *xshell = data;
	xshell->unfocus_idle = NULL;
	pid_t pid = xshell->unfocus;
	xshell->unfocus = 0;
	struct flame_view *focused = xshell->server->desktop.focused;
	pid_t now = view_pid(xshell, focused);
	if (pid && pid != now) {
		// out of the app for real: what it held goes (a game's clipped mouse)
		xserver_break_grabs(xshell->xserver, pid);
	}
	if (pid && pid != now && x_pid(xshell->xserver, xshell->xserver->focus) == pid) {
		xserver_set_focus(xshell->xserver, XS_FOCUS_NONE);
	}
}

// the app behind any window, X or wayland (steam's bridged ones), 0 unknown
static pid_t view_pid(struct flame_xshell *xshell, struct flame_view *view) {
	if (!view) {
		return 0;
	}
	if (is_xview(view)) {
		return x_pid(xshell->xserver, from_view(view)->id);
	}
	struct wlr_surface *surface = view->impl->surface ? view->impl->surface(view) : NULL;
	pid_t pid = 0;
	if (surface) {
		wl_client_get_credentials(wl_resource_get_client(surface->resource), &pid, NULL, NULL);
	}
	return pid;
}

// an X app holding the pointer has it everywhere (its menu closes on a
// click outside, a game keeps the mouse) only while it's the app the user
// is in. another workspace, a launcher over it: the rest of the desktop is
// the user's, like under Xwayland
static bool grab_honored(const struct flame_xshell *xshell) {
	if (xshell->server->layers.focused) {
		return false; // a launcher or the like over it has the user
	}
	pid_t pid = xserver_pointer_grab_pid(xshell->xserver);
	return pid && pid == view_pid((struct flame_xshell *)xshell, xshell->server->desktop.focused);
}

static void xview_set_activated(struct flame_view *view, bool activated) {
	struct flame_xwindow *window = from_view(view);
	struct flame_xshell *xshell = window->xshell;
	struct xs_window *x = x_of(window);
	if (activated) {
		// the window manager's part, ICCCM's way. focus the app moved onto
		// one of its own windows in there stays where it put it
		if (x) {
			xserver_focus_window(xshell->xserver, x);
		}
		x_stack_update(xshell);
	} else if (x && x->owner) {
		xshell->unfocus = x->owner->pid;
		if (!xshell->unfocus_idle) {
			xshell->unfocus_idle = wl_event_loop_add_idle(xshell->server->loop,
				handle_unfocus_idle, xshell);
		}
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
	window->placed = content;
	window->parked = false; // placed: it's on the workspace that shows
	// X's window around it, its shadow outside what flame shows. edges
	// rounded, not sizes: neighbours at a fractional scale don't gap
	struct flame_xshell *xshell = window->xshell;
	int x0 = xshell_to_x(xshell, content.x), y0 = xshell_to_x(xshell, content.y);
	int width = x_width(window, xshell_to_x(xshell, content.x + content.width) - x0);
	int height = x_height(window, xshell_to_x(xshell, content.y + content.height) - y0);
	if (x) {
		xserver_move_window(xshell->xserver, x, x0 - window->ext_left, y0 - window->ext_top,
			width > 0 ? width : 1, height > 0 ? height : 1);
	}
	window->width = width;
	window->height = height;
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

// _GTK_FRAME_EXTENTS: left, right, top, bottom. true when they changed.
// menus and tooltips place themselves, shadow and all
static bool read_extents(struct flame_xshell *xshell, struct flame_xwindow *window,
		struct xs_window *x) {
	int e[4] = {0, 0, 0, 0};
	struct xs_property *p = window->floating ? NULL
		: xserver_property(xshell->xserver, x, "_GTK_FRAME_EXTENTS");
	if (p && p->format == 32 && p->units >= 4) {
		uint32_t v[4];
		memcpy(v, p->data, sizeof(v));
		for (int i = 0; i < 4; i++) {
			// a shadow, not most of the window
			e[i] = v[i] < 256 ? (int)v[i] : 0;
		}
	}
	if (2 * (e[0] + e[1]) >= x->width || 2 * (e[2] + e[3]) >= x->height) {
		e[0] = e[1] = e[2] = e[3] = 0;
	}
	bool changed = e[0] != window->ext_left || e[1] != window->ext_right
		|| e[2] != window->ext_top || e[3] != window->ext_bottom;
	window->ext_left = e[0];
	window->ext_right = e[1];
	window->ext_top = e[2];
	window->ext_bottom = e[3];
	return changed;
}

// _NET_WM_WINDOW_TYPE says one of these
static bool x_typed(struct flame_xserver *xserver, struct xs_window *x,
		const char *const *types, size_t count) {
	struct xs_property *type = xserver_property(xserver, x, "_NET_WM_WINDOW_TYPE");
	for (uint32_t i = 0; type && type->format == 32 && i < type->units; i++) {
		uint32_t atom;
		memcpy(&atom, type->data + i * 4, 4);
		const char *name = xserver_atom_name(xserver, atom);
		for (size_t j = 0; name && j < count; j++) {
			if (!strcmp(name, types[j])) {
				return true;
			}
		}
	}
	return false;
}

// managed windows that float instead of tiling: dialogs, utility windows,
// splash screens, anything transient for another, anything one size only
static bool x_wants_float(struct flame_xshell *xshell, struct xs_window *x) {
	struct flame_xserver *xserver = xshell->xserver;
	if (xserver_property(xserver, x, "WM_TRANSIENT_FOR")) {
		return true;
	}
	static const char *const floaty[] = {
		"_NET_WM_WINDOW_TYPE_DIALOG", "_NET_WM_WINDOW_TYPE_UTILITY",
		"_NET_WM_WINDOW_TYPE_SPLASH", "_NET_WM_WINDOW_TYPE_TOOLBAR",
	};
	if (x_typed(xserver, x, floaty, sizeof(floaty) / sizeof(floaty[0]))) {
		return true;
	}
	// WM_NORMAL_HINTS: flags, 4 obsolete, then min and max size
	struct xs_property *hints = xserver_property(xserver, x, "WM_NORMAL_HINTS");
	if (hints && hints->format == 32 && hints->units >= 9) {
		uint32_t v[9];
		memcpy(v, hints->data, sizeof(v));
		bool min = v[0] & 16, max = v[0] & 32;
		return min && max && v[5] > 0 && v[5] == v[7] && v[6] == v[8];
	}
	return false;
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
	wl_list_init(&window->layers);
	struct flame_tree *parent;
	if (window->floating) {
		// menus and tooltips: where the app put them, above everything
		window->float_tree = scene_tree_create(server->render.layers[FLAME_LAYER_OVERLAY]);
		parent = window->float_tree;
		if (window->float_tree) {
			scene_node_set_position(&window->float_tree->node, from_x(xshell, x->x),
				from_x(xshell, x->y));
		}
	} else {
		view_init(&window->view, &xview_impl, &server->desktop);
		parent = window->view.content;
	}
	window->content = parent ? scene_tree_create(parent) : NULL;
	window->core_node = window->content
		? scene_buffer_create(window->content, &core_impl, window) : NULL;
	if (window->core_node) {
		window->core_node->input_when_empty = true;
	}
	if (!window->core_node) {
		if (window->content) {
			scene_node_destroy(&window->content->node);
		}
		if (window->float_tree) {
			scene_node_destroy(&window->float_tree->node);
		} else if (!window->floating) {
			view_finish(&window->view);
		}
		free(window);
		return NULL;
	}
	wl_list_insert(xshell->windows.prev, &window->link);
	ewmh_window_new(xshell, window, x);
	read_extents(xshell, window, x);
	if (!window->floating) {
		window->view.float_wanted = x_wants_float(xshell, x);
		desktop_map_view(&server->desktop, &window->view);
	}
	wlr_log(WLR_DEBUG, "xshell: X window 0x%x %s at %d,%d %dx%d", x->id,
		window->floating ? "floats" : "is a window", x->x, x->y, x->width, x->height);
	// drawn into before flame got to it: what's there already shows
	if (x->backing && x->painted) {
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
	wlr_buffer_finish(buffer);
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
	if (!window || self_presented(window) || !top->backing) {
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
	scene_buffer_set(window->core_node, window->core_texture,
		(const void *)++window->core_serial, from_x(xshell, pixman_image_get_width(image)),
		from_x(xshell, pixman_image_get_height(image)),
		pixman_image_get_format(image) != PIXMAN_a8r8g8b8, NULL, 0);
}

static void window_destroy(struct flame_xwindow *window) {
	struct flame_xshell *xshell = window->xshell;
	wlr_log(WLR_DEBUG, "xshell: X window 0x%x leaves flame", window->id);
	if (xshell->pointer_window == window->id) {
		xshell->pointer_window = 0;
	}
	if (!window->floating) {
		window_anim_snapshot(&window->view); // the close plays from its last picture
	}
	struct xlayer *layer, *ltmp;
	wl_list_for_each_safe(layer, ltmp, &window->layers, link) {
		layer_destroy(layer);
	}
	scene_buffer_set(window->core_node, NULL, NULL, 0, 0, false, NULL, 0);
	core_drop(window);
	scene_node_destroy(&window->content->node);
	if (window->floating) {
		scene_node_destroy(&window->float_tree->node);
	} else {
		desktop_unmap_view(&xshell->server->desktop, &window->view);
		view_finish(&window->view);
		ewmh_window_gone(xshell, window->id);
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

// ---- floating windows belong to their app's window ----

// the tiled window a floating one (a menu, a tooltip) belongs to: the window
// WM_TRANSIENT_FOR names (gtk sets it on its menus, a menu of a menu leads up
// the chain), else the same app's newest tiled window. NULL: none, it floats
// on its own
static struct flame_view *float_owner(struct flame_xshell *xshell,
		struct flame_xwindow *window) {
	struct flame_xserver *xserver = xshell->xserver;
	struct xs_window *x = xserver_window(xserver, window->id);
	struct xs_window *self = x;
	for (int depth = 0; x && depth < 8; depth++) {
		struct xs_property *p = xserver_property(xserver, x, "WM_TRANSIENT_FOR");
		uint32_t parent_id = 0;
		if (p && p->format == 32 && p->units >= 1) {
			memcpy(&parent_id, p->data, 4);
		}
		struct flame_xwindow *parent = parent_id ? window_of(xshell, parent_id) : NULL;
		if (!parent || parent == window) {
			break;
		}
		if (!parent->floating) {
			return &parent->view;
		}
		x = xserver_window(xserver, parent->id);
	}
	struct flame_view *found = NULL;
	struct flame_xwindow *other;
	wl_list_for_each(other, &xshell->windows, link) {
		struct xs_window *ox = other->floating ? NULL : xserver_window(xserver, other->id);
		if (ox && self && ox->owner && self->owner && ox->owner->pid == self->owner->pid) {
			found = &other->view;
		}
	}
	return found;
}

// a floating window shows while its owner's workspace does: a menu left open
// doesn't hang over the workspace switched to
static void floats_update(struct flame_xshell *xshell) {
	struct flame_workspace *active = xshell->server->desktop.active;
	struct flame_xwindow *window;
	wl_list_for_each(window, &xshell->windows, link) {
		if (!window->floating) {
			continue;
		}
		struct flame_view *owner = float_owner(xshell, window);
		bool show = !owner || !owner->workspace || owner->workspace == active;
		scene_node_set_enabled(&window->float_tree->node, show);
	}
}

#define PARKED (-32000)

// X sees the screen the way flame shows it, for apps that look for windows
// by X's tree (drag and drop targets, QueryPointer): tiled windows on other
// workspaces parked out of sight, the focused one on top of the tiled ones,
// menus and the like above that
static void x_stack_update(struct flame_xshell *xshell) {
	struct flame_xserver *xserver = xshell->xserver;
	struct flame_desktop *desktop = &xshell->server->desktop;
	struct flame_xwindow *window;
	wl_list_for_each(window, &xshell->windows, link) {
		struct xs_window *x = window->floating ? NULL : x_of(window);
		if (!x) {
			continue;
		}
		bool shown = window->view.workspace && window->view.workspace == desktop->active;
		if (!shown && !window->parked) {
			window->parked = true;
			xserver_move_window(xserver, x, PARKED, PARKED, x->width, x->height);
		} else if (shown && window->parked && window->placed.width > 0) {
			window->parked = false;
			xserver_move_window(xserver, x,
				xshell_to_x(xshell, window->placed.x) - window->ext_left,
				xshell_to_x(xshell, window->placed.y) - window->ext_top, x->width, x->height);
		}
	}
	// only what's out of place moves: this runs on every change, and moving
	// windows is a change. the order X should have, bottom to top: tiles
	// with the focused one last, floating windows the way flame stacks them,
	// then menus and the like in the order they have
	struct xs_window *want[512];
	size_t len = 0;
	struct xs_window *root = xserver->root;
	struct xs_window *focused = is_xview(desktop->focused)
		? x_of(from_view(desktop->focused)) : NULL;
	struct xs_window *x;
	wl_list_for_each(x, &root->children, link) {
		window = window_of(xshell, x->id);
		// floats on other workspaces are parked, they go down with the tiles
		bool shown_float = window && window->view.floating
			&& window->view.workspace == desktop->active;
		if (window && !window->floating && !shown_float && x != focused && len < 512) {
			want[len++] = x;
		}
	}
	if (focused && !from_view(desktop->focused)->view.floating && len < 512) {
		want[len++] = focused;
	}
	struct flame_view *view;
	wl_list_for_each(view, &desktop->active->floats, link) {
		x = is_xview(view) ? x_of(from_view(view)) : NULL;
		if (x && len < 512) {
			want[len++] = x;
		}
	}
	wl_list_for_each(x, &root->children, link) {
		window = window_of(xshell, x->id);
		if (window && window->floating && len < 512) {
			want[len++] = x;
		}
	}
	// where X's order first differs, everything from there gets raised in
	// the order wanted
	size_t at = 0;
	wl_list_for_each(x, &root->children, link) {
		if (at < len && x == want[at]) {
			at++;
		} else if (window_of(xshell, x->id) && x->mapped) {
			break; // one of ours out of place
		}
	}
	for (size_t i = at; i < len; i++) {
		xserver_raise_window(xserver, want[i]);
	}
}

static void handle_desktop_changed(struct wl_listener *listener, void *data) {
	struct flame_xshell *xshell = wl_container_of(listener, xshell, desktop_changed);
	floats_update(xshell);
	x_stack_update(xshell);
	ewmh_update(xshell);
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
				if (window->float_tree->node.x != from_x(xshell, x->x)
						|| window->float_tree->node.y != from_x(xshell, x->y)) {
					wlr_log(WLR_DEBUG, "xshell: floating 0x%x moves to %d,%d", x->id, x->x,
						x->y);
				}
				scene_node_set_position(&window->float_tree->node, from_x(xshell, x->x),
					from_x(xshell, x->y));
			} else {
				if (read_extents(xshell, window, x) && window->placed.width > 0
						&& !window->parked) {
					// its shadow changed (a tile has none): X's window moves
					// around what shows, which stays where it is
					xview_placed(&window->view, window->placed);
				}
				place_content(window);
				if (window->view.floating && (x->width != window->width
						|| x->height != window->height)) {
					// a floating dialog resized itself: it gets that size
					window->width = x->width;
					window->height = x->height;
					floating_content_resized(&xshell->server->desktop, &window->view,
						from_x(xshell, x->width - window->ext_left - window->ext_right),
						from_x(xshell, x->height - window->ext_top - window->ext_bottom));
				}
			}
		} else if (window) {
			window_destroy(window);
		}
	}
	layers_update(xshell);
	struct flame_xwindow *window;
	wl_list_for_each(window, &xshell->windows, link) {
		core_input(window);
	}
	floats_update(xshell);
	x_stack_update(xshell);
	ewmh_update(xshell);
	desktop_notify(&xshell->server->desktop); // titles
}

void xshell_started(struct flame_xshell *xshell) {
	ewmh_start(xshell);
	ewmh_update(xshell);
	xshell_rescale(xshell);
}

// the picture at the size it shows at now
static void content_refresh(struct flame_xwindow *window) {
	struct xlayer *layer;
	wl_list_for_each(layer, &window->layers, link) {
		layer_place(layer);
	}
	if (window->core && window->core_texture && !self_presented(window)) {
		pixman_image_t *image = window->core->image;
		scene_buffer_set(window->core_node, window->core_texture,
			(const void *)++window->core_serial,
			from_x(window->xshell, pixman_image_get_width(image)),
			from_x(window->xshell, pixman_image_get_height(image)),
			pixman_image_get_format(image) != PIXMAN_a8r8g8b8, NULL, 0);
	}
	place_content(window);
}

void xshell_rescale(struct flame_xshell *xshell) {
	const struct flame_config *config = xshell->server->config;
	struct flame_output *output = outputs_primary(&xshell->server->outputs);
	bool on = config->x_zero_scaling && output;
	double scale = on && output->wlr->scale > 0 ? output->wlr->scale : 1;
	xsettings_update(xshell, on, scale);
	if (scale == xshell->scale) {
		return;
	}
	xshell->scale = scale;
	struct flame_xwindow *window;
	wl_list_for_each(window, &xshell->windows, link) {
		struct xs_window *x = x_of(window);
		if (window->floating && x) {
			scene_node_set_position(&window->float_tree->node, from_x(xshell, x->x),
				from_x(xshell, x->y));
		} else if (window->placed.width > 0 && !window->parked) {
			xview_placed(&window->view, window->placed);
		}
		content_refresh(window);
	}
}

bool xshell_configure_request(struct flame_xshell *xshell, struct xs_window *x,
		uint16_t mask, const int32_t values[7]) {
	struct flame_xwindow *window = find_window(xshell, x->id);
	if (!window || window->floating) {
		return false; // not one flame places: as asked
	}
	// a floating one may pick its size, flame says where it goes. a tile or
	// a fullscreen window keeps what flame gave it
	if (window->view.floating && !window->view.fullscreen_wanted && (mask & (4 | 8))) {
		int width = mask & 4 ? values[2] : x->width;
		int height = mask & 8 ? values[3] : x->height;
		if (width > 0 && height > 0) {
			xserver_move_window(xshell->xserver, x, x->x, x->y, width, height);
		}
	}
	// what it has now, told the ICCCM way: an app that asked to sit at 0,0
	// learns where it really is, it places its menus from that
	xserver_configure_notify(xshell->xserver, x);
	return true;
}

void xshell_wm_message(struct flame_xshell *xshell, uint32_t window, uint32_t type,
		const uint32_t data[5]) {
	ewmh_message(xshell, window, type, data);
}

void xshell_window_unmapped(struct flame_xshell *xshell, struct xs_window *x) {
	wlr_log(WLR_DEBUG, "xshell: 0x%x unmapped", x->id);
	struct flame_xwindow *window;
	wl_list_for_each(window, &xshell->windows, link) {
		struct xlayer *layer;
		wl_list_for_each(layer, &window->layers, link) {
			struct xs_window *lx = xserver_window(xshell->xserver, layer->id);
			for (struct xs_window *w = lx; w; w = w->parent) {
				if (w == x) {
					layer_drop(layer);
					break;
				}
			}
		}
	}
}

void xshell_window_destroyed(struct flame_xshell *xshell, struct xs_window *x) {
	struct flame_xwindow *window = find_window(xshell, x->id);
	if (window) {
		window_destroy(window);
		return;
	}
	// a window inside one that presented: its pictures go back to the app
	wl_list_for_each(window, &xshell->windows, link) {
		struct xlayer *layer, *tmp;
		wl_list_for_each_safe(layer, tmp, &window->layers, link) {
			if (layer->id == x->id) {
				layer_destroy(layer);
				return;
			}
		}
	}
}

// ---- input ----

static uint16_t x_state(struct flame_xshell *xshell) {
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(xshell->server->input.seat);
	// wlroots keeps modifiers in X's own bit order, the layout goes in 13-14
	uint16_t state = keyboard ? (uint16_t)wlr_keyboard_get_modifiers(keyboard) : 0;
	uint16_t group = keyboard ? (uint16_t)((keyboard->modifiers.group & 3) << 13) : 0;
	return state | group | xshell->buttons;
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
	if (buffer->impl == &layer_impl) {
		return ((struct xlayer *)buffer->data)->window;
	}
	return buffer->impl == &core_impl ? buffer->data : NULL;
}

uint32_t xshell_window_at(struct flame_xshell *xshell, double lx, double ly) {
	struct flame_xwindow *window = window_at(xshell, pointer_snap(xshell, lx),
		pointer_snap(xshell, ly));
	return window ? window->id : 0;
}

// over X (or X holding the pointer) it looks how X says: the cursor of the
// window it's in or of the grab, a theme one, none when a game hid it
static void show_x_cursor(struct flame_xshell *xshell) {
	struct flame_cursor *cursor = &xshell->server->input.cursor;
	if (xserver_cursor_hidden(xshell->xserver)) {
		cursor_hide(cursor);
		return;
	}
	const struct xs_cursor *x = xserver_pointer_cursor(xshell->xserver);
	if (!x || x->name) {
		cursor_set_name(cursor, x ? x->name : "default");
		return;
	}
	struct flame_cursor_pixels *frames = calloc(x->len, sizeof(*frames));
	if (!frames) {
		return;
	}
	for (size_t i = 0; i < x->len; i++) {
		const struct xs_cursor_image *image = &x->images[i];
		frames[i] = (struct flame_cursor_pixels){
			.pixels = image->pixels,
			.width = image->width,
			.height = image->height,
			.hotspot_x = image->hot_x,
			.hotspot_y = image->hot_y,
			.delay = image->delay,
		};
	}
	cursor_set_pixels(cursor, x->serial, frames, x->len, (float)xshell->scale);
	free(frames);
}

void xshell_cursor_changed(struct flame_xshell *xshell) {
	if (xshell_has_pointer(xshell)) {
		show_x_cursor(xshell);
	}
}

bool xshell_pointer_motion(struct flame_xshell *xshell, double lx, double ly, uint32_t time,
		struct flame_view **view) {
	struct flame_xserver *xserver = xshell->xserver;
	*view = NULL;
	// a held button keeps feeding the window it went down on
	struct flame_xwindow *window = xshell->buttons && xshell->pointer_window
		? find_window(xshell, xshell->pointer_window)
		: window_at(xshell, pointer_snap(xshell, lx), pointer_snap(xshell, ly));
	int x = xshell_pointer_to_x(xshell, lx), y = xshell_pointer_to_x(xshell, ly);
	if (grab_honored(xshell)) {
		// an X client holds the pointer (its menu is open): everything goes
		// to it wherever the pointer is, over wayland windows too, and the
		// pointer moves no focus until it lets go, like under any X server
		xshell->pointer_window = window ? window->id : 0;
		xserver_pointer_motion(xserver, window ? x_of(window) : NULL, x, y, x_state(xshell),
			time);
		show_x_cursor(xshell);
		return true;
	}
	if (!window) {
		if (xshell->pointer_window) {
			xserver_pointer_leave(xserver, time);
			xshell->pointer_window = 0;
			// off X: flame's pointer till a wayland client sets its own
			cursor_set_name(&xshell->server->input.cursor, "default");
		}
		return false;
	}
	xshell->pointer_window = window->id;
	xserver_pointer_motion(xserver, x_of(window), x, y, x_state(xshell), time);
	show_x_cursor(xshell);
	if (xserver->pointer_window != xshell->pointer_logged) {
		// where X apps' hovers and clicks go, for when they go somewhere odd
		xshell->pointer_logged = xserver->pointer_window;
		wlr_log(WLR_DEBUG, "xshell: pointer at %.2f,%.2f (X %d,%d) over 0x%x in 0x%x%s", lx, ly, x, y,
			xserver->pointer_window, window->id, window->floating ? " (floats)" : "");
	}
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

bool xshell_menu_open(struct flame_xshell *xshell) {
	static const char *const menus[] = {
		"_NET_WM_WINDOW_TYPE_POPUP_MENU", "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU",
		"_NET_WM_WINDOW_TYPE_MENU", "_NET_WM_WINDOW_TYPE_COMBO",
	};
	struct flame_view *focused = xshell->server->desktop.focused;
	struct xs_window *app = is_xview(focused) ? x_of(from_view(focused)) : NULL;
	pid_t pid = app && app->owner ? app->owner->pid : 0;
	if (!pid) {
		return false;
	}
	struct flame_xwindow *window;
	wl_list_for_each(window, &xshell->windows, link) {
		struct xs_window *x = window->floating ? x_of(window) : NULL;
		if (x && x->owner && x->owner->pid == pid
				&& x_typed(xshell->xserver, x, menus, sizeof(menus) / sizeof(menus[0]))) {
			return true;
		}
	}
	return false;
}

bool xshell_has_pointer(const struct flame_xshell *xshell) {
	return xshell->pointer_window != 0 || grab_honored(xshell);
}

bool xshell_pointer_grabbed(const struct flame_xshell *xshell) {
	return grab_honored(xshell);
}

bool xshell_buttons_held(const struct flame_xshell *xshell) {
	return xshell->buttons != 0;
}

bool xshell_pointer_button(struct flame_xshell *xshell, uint32_t button, bool pressed,
		uint32_t time) {
	if (!xshell_has_pointer(xshell)) {
		return false;
	}
	uint8_t xbutton = x_button(button);
	if (!xbutton) {
		return true;
	}
	// a click in the window flame has focused while X's keyboard focus is
	// out of the app (it was offered with WM_TAKE_FOCUS and let it go by):
	// offered again, like X window managers do on a click
	struct flame_xwindow *window = pressed && !grab_honored(xshell)
		? find_window(xshell, xshell->pointer_window) : NULL;
	struct xs_window *x = window && &window->view == xshell->server->desktop.focused
		? x_of(window) : NULL;
	if (x && x->owner && x_pid(xshell->xserver, xshell->xserver->focus) != x->owner->pid) {
		wlr_log(WLR_DEBUG, "xshell: click in 0x%x, the keyboard isn't in it: offered again",
			x->id);
		xserver_focus_window(xshell->xserver, x);
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
	if (!xshell_has_pointer(xshell)) {
		return false;
	}
	xserver_pointer_scroll(xshell->xserver, vertical, delta, x_state(xshell), time);
	return true;
}

void xshell_pointer_raw_motion(struct flame_xshell *xshell, double dx, double dy,
		double raw_dx, double raw_dy, uint32_t time) {
	if (xshell->pointer_window || is_xview(xshell->server->desktop.focused)) {
		xserver_pointer_raw_motion(xshell->xserver, dx, dy, raw_dx, raw_dy, time);
	}
}

static uint32_t now_ms(void) {
	return (uint32_t)(now_ns() / 1000000);
}

static void handle_repeat_timer(void *data) {
	struct flame_xshell *xshell = data;
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(xshell->server->input.seat);
	if (!xshell->repeat_key || !keyboard || keyboard->repeat_info.rate <= 0
			|| !is_xview(xshell->server->desktop.focused)) {
		xshell->repeat_key = 0;
		return;
	}
	xserver_key_repeat(xshell->xserver, xshell->repeat_key, x_state(xshell), now_ms());
	timer_arm(&xshell->repeat_timer, now_ns() + 1000000000LL / keyboard->repeat_info.rate);
}

// a press starts the repeat after the keyboard's delay, unless it's a key
// that doesn't repeat (modifiers). its release, or any other press, ends it
static void repeat_update(struct flame_xshell *xshell, uint8_t keycode, bool pressed) {
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(xshell->server->input.seat);
	if (!pressed && keycode != xshell->repeat_key) {
		return;
	}
	xshell->repeat_key = 0;
	timer_arm(&xshell->repeat_timer, 0);
	if (pressed && keyboard && keyboard->keymap && keyboard->repeat_info.rate > 0
			&& xkb_keymap_key_repeats(keyboard->keymap, keycode)) {
		xshell->repeat_key = keycode;
		timer_arm(&xshell->repeat_timer,
			now_ns() + (int64_t)keyboard->repeat_info.delay * 1000000);
	}
}

bool xshell_key(struct flame_xshell *xshell, struct wlr_keyboard *keyboard, uint32_t keycode,
		bool pressed, uint32_t time) {
	if (!is_xview(xshell->server->desktop.focused)) {
		return false;
	}
	xshell_keyboard(xshell, keyboard);
	xserver_key(xshell->xserver, (uint8_t)(keycode + 8), pressed, x_state(xshell), time);
	repeat_update(xshell, (uint8_t)(keycode + 8), pressed);
	return true;
}

// a modifier key itself: shift, ctrl, alt, super, altgr
static bool modifier_key(struct wlr_keyboard *keyboard, uint32_t xkb_keycode) {
	const xkb_keysym_t *syms;
	int n = xkb_keymap_key_get_syms_by_level(keyboard->keymap, xkb_keycode, 0, 0, &syms);
	for (int i = 0; i < n; i++) {
		if ((syms[i] >= XKB_KEY_Shift_L && syms[i] <= XKB_KEY_Hyper_R)
				|| syms[i] == XKB_KEY_ISO_Level3_Shift) {
			return true;
		}
	}
	return false;
}

// what x_global_keys lets X apps see of a key typed in a wayland window
static bool x_sees_key(struct flame_xshell *xshell, struct wlr_keyboard *keyboard,
		uint32_t xkb_keycode) {
	switch (xshell->server->config->x_global_keys) {
	case FLAME_X_KEYS_NEVER:
		return false;
	case FLAME_X_KEYS_MODIFIERS:
		return modifier_key(keyboard, xkb_keycode);
	case FLAME_X_KEYS_COMBOS:
		return modifier_key(keyboard, xkb_keycode) || (wlr_keyboard_get_modifiers(keyboard)
			& (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO));
	case FLAME_X_KEYS_ALWAYS:
		return true;
	}
	return false;
}

bool xshell_key_elsewhere(struct flame_xshell *xshell, struct wlr_keyboard *keyboard,
		uint32_t keycode, bool pressed, uint32_t time) {
	uint8_t x = (uint8_t)(keycode + 8);
	// a press as the setting says. a release when X saw the key go down,
	// whatever the setting says now: nothing stays held in obs or a push to
	// talk, and no release for a key X never had (a flame bind's)
	if (pressed ? !x_sees_key(xshell, keyboard, x) : !xserver_key_down(xshell->xserver, x)) {
		return false;
	}
	xshell_keyboard(xshell, keyboard);
	uint16_t state = (uint16_t)wlr_keyboard_get_modifiers(keyboard)
		| (uint16_t)((keyboard->modifiers.group & 3) << 13) | xshell->buttons;
	return xserver_key_elsewhere(xshell->xserver, x, pressed, state, time);
}

void xshell_button_elsewhere(struct flame_xshell *xshell, uint32_t button, bool pressed,
		uint32_t time) {
	uint8_t xbutton = x_button(button);
	if (xbutton && xshell->server->config->x_global_keys == FLAME_X_KEYS_ALWAYS) {
		xserver_button_elsewhere(xshell->xserver, xbutton, pressed, time);
	}
}

void xshell_keyboard(struct flame_xshell *xshell, struct wlr_keyboard *keyboard) {
	struct flame_xserver *xserver = xshell->xserver;
	if (keyboard->keymap) {
		xserver_set_keymap(xserver, keyboard->keymap);
	}
	xserver_set_repeat(xserver, keyboard->repeat_info.rate, keyboard->repeat_info.delay);
	xserver_keyboard_state(xserver, keyboard->modifiers.depressed, keyboard->modifiers.latched,
		keyboard->modifiers.locked, keyboard->modifiers.group);
}

// ---- the gpu ----

const char *xshell_gl_vendor(struct flame_xshell *xshell) {
	// like Xwayland: the kernel driver of the gpu flame renders on decides
	int drm_fd = wlr_renderer_get_drm_fd(xshell->server->render.renderer);
	drmVersion *version = drm_fd >= 0 ? drmGetVersion(drm_fd) : NULL;
	bool nvidia = version && version->name && !strcmp(version->name, "nvidia-drm");
	drmFreeVersion(version);
	return nvidia ? "nvidia" : "mesa";
}

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
	xshell->scale = 1;
	wl_list_init(&xshell->windows);
	wl_list_init(&xshell->frames);
	wl_list_init(&xshell->notifies);
	if (!timer_init(&xshell->msc_timer, server->loop, handle_msc_timer, xshell)) {
		return false;
	}
	if (!timer_init(&xshell->repeat_timer, server->loop, handle_repeat_timer, xshell)) {
		timer_finish(&xshell->msc_timer);
		return false;
	}
	listener_connect(&xshell->new_output, &server->outputs.events.new_frame,
		handle_new_frame);
	listener_connect(&xshell->desktop_changed, &server->desktop.events.changed,
		handle_desktop_changed);
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
	if (xshell->unfocus_idle) {
		wl_event_source_remove(xshell->unfocus_idle);
	}
	if (xshell->copy_timeline) {
		wlr_drm_syncobj_timeline_unref(xshell->copy_timeline);
	}
	timer_finish(&xshell->msc_timer);
	timer_finish(&xshell->repeat_timer);
	listener_disconnect(&xshell->new_output);
	listener_disconnect(&xshell->desktop_changed);
}
