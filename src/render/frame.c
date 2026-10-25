#include "render/frame.h"

#include <math.h>
#include <wlr/backend.h>
#include <wlr/backend/session.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_tearing_control_v1.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>
#include <wlr/util/transform.h>

#include "config/config.h"
#include "core/server.h"
#include "render/coords.h"
#include "render/damage.h"
#include "render/feedback.h"
#include "render/pass.h"
#include "render/render.h"
#include "scene/scene.h"
#include "util/listener.h"

static void process(struct flame_frame *frame);
static void process_once(struct flame_frame *frame);
static void vrr_refused(struct flame_frame *frame);

static int64_t timespec_ns(const struct timespec *ts) {
	return (int64_t)ts->tv_sec * 1000000000 + ts->tv_nsec;
}

static struct timespec ns_timespec(int64_t ns) {
	return (struct timespec){
		.tv_sec = ns / 1000000000,
		.tv_nsec = ns % 1000000000,
	};
}

static int64_t refresh_period(struct wlr_output *output) {
	// mHz, 0 when the mode doesn't say (headless)
	return output->refresh > 0 ? 1000000000000LL / output->refresh : 0;
}

// ---- asking for frames ----

static void handle_idle(void *data) {
	struct flame_frame *frame = data;
	frame->idle = NULL;
	process(frame);
}

// everything asking in one dispatch becomes one look at the schedule, at
// the end of it, after input and clients had their say
static void queue_process(struct flame_frame *frame) {
	if (frame->processing) {
		return; // its timer covers it, see process()
	}
	if (!frame->idle) {
		frame->idle = wl_event_loop_add_idle(frame->output->event_loop, handle_idle, frame);
	}
}

void frame_display_changed(struct flame_frame *frame) {
	sync_reset(&frame->sync); // KMS gets another chance at tearing and vrr
	frame->tearing_logged = false;
	frame->vrr_logged = false;
	frame_request(frame);
}

void frame_request(struct flame_frame *frame) {
	schedule_request_redraw(&frame->schedule);
	queue_process(frame);
}

static void handle_wake(void *data) {
	struct flame_frame *frame = data;
	timing_wake(&frame->timing, now_ns() - frame->wake.fired);
	process(frame);
}

// ---- what wakes us up ----

// the lists keep surface pointers from the last frames: callbacks go out
// from them until the next render, a dead one would be a use after free
static void handle_surface_destroy(struct wl_listener *listener, void *data) {
	struct flame_frame *frame = wl_container_of(listener, frame, surface_destroy);
	elements_forget_surface(&frame->prev, data);
	elements_forget_surface(&frame->cur, data);
}

static void handle_scene_changed(struct wl_listener *listener, void *data) {
	struct flame_frame *frame = wl_container_of(listener, frame, scene_changed);
	frame_request(frame);
}

static void handle_scene_damage(struct wl_listener *listener, void *data) {
	struct flame_frame *frame = wl_container_of(listener, frame, scene_damage);
	const pixman_region32_t *damage = data;
	pixman_region32_union(&frame->pending, &frame->pending, damage);
	// scene_changed follows and asks for the frame
}

// the pointer moved or looks different. on the plane that's a cheap commit,
// drawn it's a small repaint, the frame works out which
static void handle_pointer_changed(struct wl_listener *listener, void *data) {
	struct flame_frame *frame = wl_container_of(listener, frame, pointer_changed);
	frame_request(frame);
}

// wlroots itself wants a real commit (screencopy and friends). flame never
// asks wlroots for frames, so this is always someone else
static void handle_output_needs_frame(struct wl_listener *listener, void *data) {
	struct flame_frame *frame = wl_container_of(listener, frame, output_needs_frame);
	frame_request(frame);
}

static void drop_queued(struct flame_frame *frame) {
	if (frame->has_queued) {
		wlr_output_state_finish(&frame->queued);
		frame->has_queued = false;
	}
}

// someone else put a buffer on the output: its flip is pending like one of
// ours, committing over it would only get refused as busy
static void adopt_foreign_commit(struct flame_frame *frame) {
	if (frame->schedule.active && !frame->schedule.pending) {
		int64_t now = now_ns();
		// a frame asked for (startup's first) still is: that commit isn't ours
		bool redraw = frame->schedule.redraw;
		schedule_submitted(&frame->schedule, &frame->foreign_ticket, now, now);
		frame->schedule.redraw = redraw;
	}
}

static void handle_output_commit(struct wl_listener *listener, void *data) {
	struct flame_frame *frame = wl_container_of(listener, frame, output_commit);
	const struct wlr_output_event_commit *event = data;
	if (!frame->committing && (event->state->committed & WLR_OUTPUT_STATE_BUFFER)) {
		adopt_foreign_commit(frame);
	}
	// a different size or orientation leaves nothing in the old buffers usable
	if (event->state->committed & (WLR_OUTPUT_STATE_MODE | WLR_OUTPUT_STATE_SCALE
			| WLR_OUTPUT_STATE_TRANSFORM | WLR_OUTPUT_STATE_ENABLED)) {
		frame->whole = true;
		schedule_set_period(&frame->schedule, refresh_period(frame->output));
		frame_display_changed(frame);
	}
}

// ---- watching a stretch of frames ----

static void watch_report(struct flame_frame *frame);
static int handle_watch_done(void *data);

void frame_watch(struct flame_frame *frame, const char *what, int64_t ns) {
	watch_report(frame); // one still going: what it has so far
	if (!frame->watch.done) {
		frame->watch.done = wl_event_loop_add_timer(frame->output->event_loop,
			handle_watch_done, frame);
	}
	if (frame->watch.done) {
		wl_event_source_timer_update(frame->watch.done, (int)(ns / 1000000) + 1);
	}
	frame->watch.on = true;
	snprintf(frame->watch.what, sizeof(frame->watch.what), "%s", what);
	frame->watch.started = now_ns();
	frame->watch.until = frame->watch.started + ns;
	frame->watch.last_flip = 0;
	frame->watch.frames = frame->watch.missed = 0;
	frame->watch.worst_gap = frame->watch.worst_gpu = 0;
	frame->watch.worst_own_ms = 0;
	frame->watch.drawn = frame->render->gl.stats;
	frame->watch.composited = 0;
}

// the watch is over: its line, the wall clock in it to line up with what
// else was going on (gpu clocks)
static void watch_report(struct flame_frame *frame) {
	if (!frame->watch.on) {
		return;
	}
	frame->watch.on = false;
	struct timespec wall;
	clock_gettime(CLOCK_REALTIME, &wall);
	struct tm tm;
	localtime_r(&wall.tv_sec, &tm);
	double span = (double)(frame->watch.until - frame->watch.started) / 1e6;
	double refresh = frame->output->refresh > 0 ? frame->output->refresh / 1000.0 : 0;
	// done: from going to the gpu to finished, waiting behind others included.
	// own: the gpu's time on flame's commands alone (an app's work cutting in
	// between them counts too, so it's at most that)
	wlr_log(WLR_INFO, "%s: %s: %d frames in %.0fms (%.0f at %.1fHz), %d vblanks missed, "
		"worst gap %.1fms, gpu done in up to %.1fms, flame's own gpu work up to %.1fms "
		"[%02d:%02d:%02d.%03ld]",
		frame->output->name, frame->watch.what, frame->watch.frames, span,
		refresh * span / 1000, refresh, frame->watch.missed, frame->watch.worst_gap / 1e6,
		frame->watch.worst_gpu / 1e6, frame->watch.worst_own_ms, tm.tm_hour, tm.tm_min,
		tm.tm_sec, wall.tv_nsec / 1000000);
	// what a drawn frame asked of the gpu, in thousands of pixels: the same
	// whatever its clocks
	const struct gl_draw_stats *now = &frame->render->gl.stats, *then = &frame->watch.drawn;
	int n = frame->watch.composited > 0 ? frame->watch.composited : 1;
	wlr_log(WLR_INFO, "%s: %s: a drawn frame was %.0fk pixels: windows and pictures %.0fk, "
		"plain %.0fk, glass %.0fk, its blur %.0fk, copied for it %.0fk, %.1f blurs of %.1f "
		"passes (%d frames drawn)",
		frame->output->name, frame->watch.what,
		(double)((now->texture - then->texture) + (now->rect - then->rect)
			+ (now->glass - then->glass) + (now->blur - then->blur)
			+ (now->copy - then->copy)) / n / 1000,
		(double)(now->texture - then->texture) / n / 1000,
		(double)(now->rect - then->rect) / n / 1000,
		(double)(now->glass - then->glass) / n / 1000,
		(double)(now->blur - then->blur) / n / 1000,
		(double)(now->copy - then->copy) / n / 1000,
		(double)(now->chains - then->chains) / n, (double)(now->steps - then->steps) / n,
		frame->watch.composited);
}

static int handle_watch_done(void *data) {
	watch_report(data);
	return 0;
}

// a flip landed while watching: counted
static void watch_flip(struct flame_frame *frame, const struct wlr_output_event_present *event) {
	if (!frame->watch.on || !event->presented) {
		return;
	}
	int64_t when = timespec_ns(&event->when);
	if (when > frame->watch.until) {
		return; // the timer has it
	}
	if (frame->watch.last_flip) {
		int64_t gap = when - frame->watch.last_flip;
		frame->watch.worst_gap = gap > frame->watch.worst_gap ? gap : frame->watch.worst_gap;
		if (event->seq > frame->watch.last_seq + 1) {
			frame->watch.missed += (int)(event->seq - frame->watch.last_seq - 1);
		}
	}
	frame->watch.last_flip = when;
	frame->watch.last_seq = event->seq;
	frame->watch.frames++;
}

static void handle_output_present(struct wl_listener *listener, void *data) {
	struct flame_frame *frame = wl_container_of(listener, frame, output_present);
	const struct wlr_output_event_present *event = data;
	watch_flip(frame, event);
	// a flip without a timestamp anchors on dispatch time instead
	frame->present_ns = event->presented ? timespec_ns(&event->when) : 0;
	if (frame->vrr_switch_since && event->presented) {
		wlr_log(WLR_INFO, "%s: the adaptive sync switch was on screen %.1f ms after it went out",
			frame->output->name, (timespec_ns(&event->when) - frame->vrr_switch_since) / 1e6);
		frame->vrr_switch_since = 0;
	}
	// wayland buffers drawn into that frame made it now
	int kept = 0;
	for (int i = 0; i < frame->first_drawn_len; i++) {
		if (frame->pending_draw && frame->first_drawn[i].draw_id <= frame->pending_draw) {
			if (event->presented) {
				timing_latency(&frame->timing, false,
					timespec_ns(&event->when) - frame->first_drawn[i].since);
			}
		} else {
			frame->first_drawn[kept++] = frame->first_drawn[i];
		}
	}
	frame->first_drawn_len = kept;
	// buffer nodes drawn into that frame hear it made it
	if (frame->pending_draw) {
		struct flame_frame_presented presented = {
			.draw_id = frame->pending_draw,
			.presented = event->presented,
			.when = timespec_ns(&event->when),
			.seq = event->seq,
			.refresh = event->refresh,
		};
		frame->pending_draw = 0;
		wl_signal_emit_mutable(&frame->events.presented, &presented);
	}
}

// torn: it went as a tearing flip (only asked for with tear)
static bool commit(struct flame_frame *frame, struct wlr_output_state *state,
		enum timing_commit kind, bool tear, bool *torn) {
	if (torn) {
		*torn = false;
	}
	if (tear) {
		// an async flip changes the buffer and nothing else: KMS refuses one
		// carrying damage clips on most drivers
		state->tearing_page_flip = true;
		state->committed &= ~WLR_OUTPUT_STATE_DAMAGE;
	}
	frame->committing = true;
	bool ok = wlr_output_commit_state(frame->output, state);
	if (tear && !ok) {
		// KMS or the driver wouldn't tear this one: a plain flip right away,
		// nothing stalls. too many of those and tearing's off for a while
		state->tearing_page_flip = false;
		ok = wlr_output_commit_state(frame->output, state);
		sync_tearing_result(&frame->sync, true);
		if (frame->sync.tearing_off && !frame->tearing_logged) {
			wlr_log(WLR_INFO, "%s: KMS keeps refusing tearing flips, vsync until the next "
				"mode change or vt switch", frame->output->name);
			frame->tearing_logged = true;
		}
	} else if (tear) {
		sync_tearing_result(&frame->sync, false);
		if (torn) {
			*torn = true;
		}
		timing_tearing(&frame->timing);
	}
	frame->committing = false;
	if (!ok) {
		wlr_log(WLR_DEBUG, "commit on %s failed", frame->output->name);
	} else {
		// KMS took the cursor plane's changes along with it
		frame_cursor_committed(&frame->cursor);
		if (state->committed & WLR_OUTPUT_STATE_BUFFER) {
			struct wlr_buffer *front = wlr_buffer_lock(state->buffer);
			if (frame->front) {
				wlr_buffer_unlock(frame->front);
			}
			frame->front = front;
		}
	}
	if (!ok && kind == TIMING_SCANOUT && !frame->scanout.suspended) {
		// it passed the test and still got refused: one full composition now,
		// and no scanout attempts until the next vt switch
		wlr_log(WLR_INFO, "%s: KMS refused a scanout frame, composing until the next vt switch",
			frame->output->name);
		frame->scanout.suspended = true;
		frame->whole = true;
		schedule_request_redraw(&frame->schedule);
	}
	timing_committed(&frame->timing, ok, kind);
	return ok;
}

// the KMS slot is free again: the pending frame flipped. a successor goes
// right in. wlroots also sends this when someone else asked for a frame with
// nothing of ours pending, that's just a request
static void handle_output_frame(struct wl_listener *listener, void *data) {
	struct flame_frame *frame = wl_container_of(listener, frame, output_frame);
	struct schedule *schedule = &frame->schedule;
	int64_t now = now_ns();

	if (!schedule->pending) {
		schedule_request_redraw(schedule);
		process(frame);
		return;
	}
	schedule_presented(schedule, frame->present_ns, now);
	if (schedule->last_missed) {
		timing_missed(&frame->timing);
	}
	timing_late_app_frames(&frame->timing, frame->render->scene.late_app_frames);
	frame->render->scene.late_app_frames = 0;
	frame->present_ns = 0;

	if (frame->has_queued) {
		bool vrr_asked = frame->queued.committed & WLR_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED;
		bool vrr_want = frame->queued.adaptive_sync_enabled;
		bool ok = commit(frame, &frame->queued, frame->queued_kind, false, NULL);
		drop_queued(frame);
		if (ok && vrr_asked && vrr_want
				!= (frame->output->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED)) {
			vrr_refused(frame);
		}
		if (ok) {
			frame->pending_draw = frame->queued_draw;
			schedule_promoted(schedule, now_ns());
			timing_successor(&frame->timing, true);
		} else {
			schedule_discard_queued(schedule);
			timing_successor(&frame->timing, false);
			frame->whole = true;
		}
	}
	process(frame);
}

// vt switches: nothing reaches KMS while we're away, and a flip that was in
// flight never reports back. start clean when we're back
static void handle_session_active(struct wl_listener *listener, void *data) {
	struct flame_frame *frame = wl_container_of(listener, frame, session_active);
	struct wlr_session *session = data;
	if (!session->active) {
		void *pending, *queued;
		schedule_pause(&frame->schedule, &pending, &queued);
		drop_queued(frame);
		timer_arm(&frame->wake, 0);
		return;
	}
	schedule_resume(&frame->schedule);
	frame->whole = true;
	frame->scanout.suspended = false;
	sync_reset(&frame->sync);
	frame->tearing_logged = false;
	frame->vrr_logged = false;
	frame_cursor_reset(&frame->cursor);
	queue_process(frame);
}

// ---- apps' buffers the gpu still reads ----

struct flame_held_read {
	struct wlr_buffer *buffer;
	uint64_t point; // on in_timeline
};

static void watch_held(struct flame_frame *frame);

// everything read by point and before is done with
static void let_go_held(struct flame_frame *frame, uint64_t point) {
	if (frame->held_waiting) {
		wlr_drm_syncobj_timeline_waiter_finish(&frame->held_waiter);
		frame->held_waiting = false;
	}
	struct flame_held_read *reads = frame->held.data;
	size_t len = frame->held.size / sizeof(*reads), kept = 0;
	for (size_t i = 0; i < len; i++) {
		if (reads[i].point <= point) {
			wlr_buffer_unlock(reads[i].buffer);
		} else {
			reads[kept++] = reads[i];
		}
	}
	frame->held.size = kept * sizeof(*reads);
	watch_held(frame);
}

static void handle_held_done(struct wlr_drm_syncobj_timeline_waiter *waiter) {
	struct flame_frame *frame = wl_container_of(waiter, frame, held_waiter);
	struct flame_held_read *reads = frame->held.data;
	let_go_held(frame, reads[0].point); // the oldest is what was watched
}

// points only go up, the oldest held is the first
static void watch_held(struct flame_frame *frame) {
	if (frame->held_waiting || frame->held.size == 0 || !frame->in_timeline) {
		return;
	}
	struct flame_held_read *reads = frame->held.data;
	frame->held_waiting = wlr_drm_syncobj_timeline_waiter_init(&frame->held_waiter,
		frame->in_timeline, reads[0].point, 0, frame->output->event_loop, handle_held_done);
	if (!frame->held_waiting) {
		// can't hear when: the old way, let go now
		for (size_t i = 0; i < frame->held.size / sizeof(*reads); i++) {
			wlr_buffer_unlock(reads[i].buffer);
		}
		frame->held.size = 0;
	}
}

// surface's buffer is read by this frame's gpu work
static void hold_read(struct flame_frame *frame, struct wlr_surface *surface) {
	if (!frame->in_timeline || !surface->buffer
			|| wlr_linux_drm_syncobj_v1_get_surface_state(surface)) {
		return; // explicit sync has its release point, see sampled()
	}
	// shm is copied into a texture of ours, only a dmabuf is read in place
	struct wlr_dmabuf_attributes dmabuf;
	if (!surface->buffer->source
			|| !wlr_buffer_get_dmabuf(surface->buffer->source, &dmabuf)) {
		return;
	}
	struct flame_held_read *read = wl_array_add(&frame->held, sizeof(*read));
	if (!read) {
		return;
	}
	read->buffer = wlr_buffer_lock(&surface->buffer->base);
	read->point = frame->in_point;
}

// ---- setup ----

bool frame_init(struct flame_frame *frame, struct flame_render *render,
		struct wlr_output *output, struct wlr_session *session, const struct wlr_box *area,
		struct flame_frame_hooks hooks) {
	frame->output = output;
	frame->render = render;
	frame->area = area;
	frame->hooks = hooks;
	frame->whole = true;
	frame->callback_cycle = 1;
	wl_signal_init(&frame->events.presented);
	wl_signal_init(&frame->events.destroy);
	wl_array_init(&frame->held);
	wlr_damage_ring_init(&frame->ring);
	pixman_region32_init(&frame->pending);
	elements_init(&frame->prev);
	elements_init(&frame->cur);
	schedule_init(&frame->schedule, render->pipeline, refresh_period(output));
	sync_init(&frame->sync);
	scanout_init(&frame->scanout, render, output);
	frame_cursor_init(&frame->cursor, render, output);
	timing_init(&frame->timing, output, &frame->schedule, render->frame_timing);
	// the schedule's lead needs the gpu's side too, immediate doesn't look at it
	if (render->frame_timing || render->pipeline != SCHEDULE_IMMEDIATE) {
		gpu_time_init(&frame->gpu, render->renderer);
	}
	if (!timer_init(&frame->wake, output->event_loop, handle_wake, frame)) {
		wlr_log(WLR_ERROR, "failed to create a frame timer for %s", output->name);
		frame_finish(frame);
		return false;
	}

	// same condition as clients' explicit sync: both ends have to speak it
	int drm_fd = wlr_backend_get_drm_fd(output->backend);
	if (drm_fd >= 0 && output->backend->features.timeline
			&& render->renderer->features.timeline) {
		frame->in_timeline = wlr_drm_syncobj_timeline_create(drm_fd);
		frame->out_timeline = wlr_drm_syncobj_timeline_create(drm_fd);
		if (!frame->in_timeline || !frame->out_timeline) {
			wlr_log(WLR_ERROR, "failed to create output timelines");
			frame_finish(frame);
			return false;
		}
	}

	// enabled means a buffer went in with the modeset, its flip is still on the way
	if (output->enabled) {
		adopt_foreign_commit(frame);
	}

	listener_connect(&frame->scene_changed, &render->scene.events.changed,
		handle_scene_changed);
	listener_connect(&frame->scene_damage, &render->scene.events.damage, handle_scene_damage);
	listener_connect(&frame->surface_destroy, &render->scene.events.surface_destroy,
		handle_surface_destroy);
	listener_connect(&frame->pointer_changed, &render->pointer.events.changed,
		handle_pointer_changed);
	listener_connect(&frame->output_needs_frame, &output->events.needs_frame,
		handle_output_needs_frame);
	listener_connect(&frame->output_commit, &output->events.commit, handle_output_commit);
	listener_connect(&frame->output_frame, &output->events.frame, handle_output_frame);
	listener_connect(&frame->output_present, &output->events.present, handle_output_present);
	if (session) {
		listener_connect(&frame->session_active, &session->events.active,
			handle_session_active);
	} else {
		wl_list_init(&frame->session_active.link);
	}
	queue_process(frame); // the first frame
	return true;
}

void frame_finish(struct flame_frame *frame) {
	if (frame->events.destroy.listener_list.next) {
		wl_signal_emit_mutable(&frame->events.destroy, frame);
	}
	if (frame->done_waiting) {
		wlr_drm_syncobj_timeline_waiter_finish(&frame->done_waiter);
		frame->done_waiting = false;
	}
	if (frame->watch.done) {
		wl_event_source_remove(frame->watch.done);
		frame->watch.done = NULL;
	}
	let_go_held(frame, UINT64_MAX);
	wl_array_release(&frame->held);
	wl_array_init(&frame->held);
	if (frame->scene_changed.link.next) {
		listener_disconnect(&frame->scene_changed);
		listener_disconnect(&frame->scene_damage);
		listener_disconnect(&frame->surface_destroy);
		listener_disconnect(&frame->pointer_changed);
		listener_disconnect(&frame->output_needs_frame);
		listener_disconnect(&frame->output_commit);
		listener_disconnect(&frame->output_frame);
		listener_disconnect(&frame->output_present);
		listener_disconnect(&frame->session_active);
	}
	if (frame->idle) {
		wl_event_source_remove(frame->idle);
		frame->idle = NULL;
	}
	timer_finish(&frame->wake);
	drop_queued(frame);
	scanout_finish(&frame->scanout);
	frame_cursor_finish(&frame->cursor);
	if (frame->front) {
		wlr_buffer_unlock(frame->front);
		frame->front = NULL;
	}
	if (frame->snapshots) {
		wlr_swapchain_destroy(frame->snapshots);
		frame->snapshots = NULL;
	}
	timing_finish(&frame->timing);
	gpu_time_finish(&frame->gpu);
	if (frame->in_timeline) {
		// nothing waits on it after this, let anything blocked on it go
		wlr_drm_syncobj_timeline_signal(frame->in_timeline, UINT64_MAX);
		wlr_drm_syncobj_timeline_unref(frame->in_timeline);
		frame->in_timeline = NULL;
	}
	if (frame->out_timeline) {
		wlr_drm_syncobj_timeline_unref(frame->out_timeline);
		frame->out_timeline = NULL;
	}
	elements_finish(&frame->prev);
	elements_finish(&frame->cur);
	pixman_region32_fini(&frame->pending);
	wlr_damage_ring_finish(&frame->ring);
}

// ---- drawing ----

static void swap_lists(struct flame_frame *frame) {
	struct flame_element_list tmp = frame->prev;
	frame->prev = frame->cur;
	frame->cur = tmp;
}

// a client buffer we read from: when our drawing finishes it can have it
// back, and its presentation feedback rides on this commit
static void sampled(struct flame_frame *frame, struct wlr_surface *surface) {
	wlr_presentation_surface_textured_on_output(surface, frame->output);
	struct wlr_linux_drm_syncobj_surface_v1_state *sync =
		wlr_linux_drm_syncobj_v1_get_surface_state(surface);
	if (sync && frame->in_timeline) {
		wlr_linux_drm_syncobj_v1_state_add_release_point(sync, frame->in_timeline,
			frame->in_point, frame->output->event_loop);
	}
}

static void draw_element(struct flame_frame *frame, struct flame_pass *pass,
		const struct flame_render_target *target, const struct flame_element *element,
		const pixman_region32_t *repaint) {
	// only the part that shows and changed
	pixman_region32_t region;
	pixman_region32_init(&region);
	pixman_region32_copy(&region, &element->visible);
	target_region_to_buffer(target, &region, true);
	pixman_region32_intersect(&region, &region, repaint);
	if (!pixman_region32_not_empty(&region)) {
		pixman_region32_fini(&region);
		return;
	}

	if (element->clear) {
		// nothing to draw, but it's shown
		sampled(frame, element->surface);
		pixman_region32_fini(&region);
		return;
	}

	struct wlr_box dst = element->box;
	target_box_to_buffer(target, &dst);

	if (element->type == FLAME_ELEMENT_GLASS) {
		pass_glass_style(pass, &element->glass, dst, &region, target->scale, element->alpha);
		pixman_region32_fini(&region);
		return;
	}
	if (element->type == FLAME_ELEMENT_RECT) {
		// premultiplied: fading scales every channel
		float alpha = element->alpha;
		const float color[4] = {element->color[0] * alpha, element->color[1] * alpha,
			element->color[2] * alpha, element->color[3] * alpha};
		pass_rect(pass, dst, color, &region, true);
		pixman_region32_fini(&region);
		return;
	}

	// fully opaque where we draw: no blending needed
	pixman_region32_t translucent;
	pixman_region32_init(&translucent);
	pixman_region32_copy(&translucent, &element->opaque);
	target_region_to_buffer(target, &translucent, false);
	pixman_region32_subtract(&translucent, &region, &translucent);
	bool blend = pixman_region32_not_empty(&translucent) || element->alpha < 1.0f;
	pixman_region32_fini(&translucent);

	// textures of ours (the pointer) have nothing to wait for, buffer nodes
	// (X11 windows) carry their own acquire point
	bool client = element->type == FLAME_ELEMENT_SURFACE;
	struct wlr_linux_drm_syncobj_surface_v1_state *sync =
		client ? wlr_linux_drm_syncobj_v1_get_surface_state(element->surface) : NULL;
	struct wlr_drm_syncobj_timeline *wait_timeline =
		sync ? sync->acquire_timeline : element->wait_timeline;
	uint64_t wait_point = sync ? sync->acquire_point : element->wait_point;
	enum wl_output_transform transform = wlr_output_transform_compose(
		wlr_output_transform_invert(element->transform), target->transform);
	// a rounded window's surface: cut to its corners
	struct wlr_box shape = element->shape;
	if (element->shape_radius > 0) {
		target_box_to_buffer(target, &shape);
	}
	pass_texture(pass, &(struct gl_texture_options){
		.texture = element->texture,
		.src = element->src,
		.dst = dst,
		.transform = transform,
		.clip = &region,
		.alpha = element->alpha,
		.blend = blend,
		.wait_timeline = wait_timeline,
		.wait_point = wait_point,
		.shape = shape,
		.shape_radius = element->shape_radius * target->scale,
	});
	if (client) {
		sampled(frame, element->surface);
		hold_read(frame, element->surface);
		// a new buffer's first time on screen, for the latency probe
		struct flame_surface_state *state = scene_surface_state(element->surface);
		if (state && state->buffer_since && frame->timing.enabled
				&& frame->first_drawn_len < (int)(sizeof(frame->first_drawn)
					/ sizeof(frame->first_drawn[0]))) {
			frame->first_drawn[frame->first_drawn_len].draw_id = frame->draw_id;
			frame->first_drawn[frame->first_drawn_len].since = state->buffer_since;
			frame->first_drawn_len++;
			state->buffer_since = 0;
		}
	} else if (element->buffer_node && element->buffer_node->impl
			&& element->buffer_node->impl->sampled) {
		element->buffer_node->impl->sampled(element->buffer_node, frame, frame->draw_id,
			frame->in_timeline, frame->in_point);
	}
	pixman_region32_fini(&region);
}

// black where nothing opaque covers the repaint area
static void draw_background(struct flame_pass *pass,
		const struct flame_render_target *target, const struct flame_element_list *list,
		const pixman_region32_t *repaint, int width, int height) {
	pixman_region32_t background;
	pixman_region32_init(&background);
	pixman_region32_copy(&background, repaint);
	for (size_t i = 0; i < list->len; i++) {
		const struct flame_element *element = &list->data[i];
		pixman_region32_t covered;
		pixman_region32_init(&covered);
		pixman_region32_intersect(&covered, &element->opaque, &element->visible);
		target_region_to_buffer(target, &covered, false);
		pixman_region32_subtract(&background, &background, &covered);
		pixman_region32_fini(&covered);
	}
	if (target->scale != (float)(int)target->scale) {
		// fractional edges, a pixel of black under them is safer than garbage
		wlr_region_expand(&background, &background, 1);
		pixman_region32_intersect(&background, &background, repaint);
	}
	pass_rect(pass, (struct wlr_box){.width = width, .height = height},
		(const float[4]){0, 0, 0, 1}, &background, false);
	pixman_region32_fini(&background);
}

// glass is drawn from what's in the buffer under it: wherever one gets
// drawn, everything it reads has to be drawn fresh this frame first, or it
// blurs its own last frame into itself. repaint grows to that (buffer
// coords), until no glass reaches past it. it's only redrawn, not changed:
// the damage ring doesn't hear of it
static void glass_repaint(const struct flame_element_list *list,
		const struct flame_render_target *target, pixman_region32_t *repaint, int width,
		int height) {
	for (size_t round = 0; round <= list->len; round++) {
		bool grew = false;
		for (size_t i = 0; i < list->len; i++) {
			const struct flame_element *element = &list->data[i];
			if (element->type != FLAME_ELEMENT_GLASS) {
				continue;
			}
			struct wlr_box box = element->clip;
			target_box_to_buffer(target, &box);
			pixman_box32_t shows = {box.x, box.y, box.x + box.width, box.y + box.height};
			if (pixman_region32_contains_rectangle(repaint, &shows) == PIXMAN_REGION_OUT) {
				continue;
			}
			int grow = (int)ceilf(elements_glass_reach(element) * target->scale);
			pixman_region32_t reach;
			pixman_region32_init_rect(&reach, box.x - grow, box.y - grow,
				(unsigned)(box.width + 2 * grow), (unsigned)(box.height + 2 * grow));
			pixman_region32_intersect_rect(&reach, &reach, 0, 0, width, height);
			pixman_region32_t missing;
			pixman_region32_init(&missing);
			pixman_region32_subtract(&missing, &reach, repaint);
			if (pixman_region32_not_empty(&missing)) {
				pixman_region32_union(repaint, repaint, &reach);
				grew = true;
			}
			pixman_region32_fini(&missing);
			pixman_region32_fini(&reach);
		}
		if (!grew) {
			break;
		}
	}
}

// renders the frame into a buffer and fills state with it, ready to commit
static uint64_t region_area(const pixman_region32_t *region) {
	int count = 0;
	const pixman_box32_t *boxes = pixman_region32_rectangles((pixman_region32_t *)region, &count);
	uint64_t area = 0;
	for (int i = 0; i < count; i++) {
		area += (uint64_t)(boxes[i].x2 - boxes[i].x1) * (uint64_t)(boxes[i].y2 - boxes[i].y1);
	}
	return area;
}

static bool draw(struct flame_frame *frame, const struct flame_render_target *target,
		const pixman_region32_t *damage, struct wlr_output_state *state) {
	struct wlr_output *output = frame->output;
	if (!wlr_output_configure_primary_swapchain(output, state, &output->swapchain)) {
		wlr_log(WLR_ERROR, "no swapchain for %s", output->name);
		return false;
	}
	struct wlr_buffer *buffer = wlr_swapchain_acquire(output->swapchain);
	if (!buffer) {
		return false;
	}

	// this frame's damage, grown by what this buffer missed since it was last used
	pixman_region32_t repaint;
	pixman_region32_init(&repaint);
	wlr_damage_ring_add(&frame->ring, damage);
	wlr_damage_ring_rotate_buffer(&frame->ring, buffer, &repaint);
	glass_repaint(&frame->cur, target, &repaint, buffer->width, buffer->height);
	if (frame->timing.enabled) {
		timing_areas(&frame->timing, region_area(damage), region_area(&repaint));
	}

	frame->in_point++;
	frame->draw_id++;
	// everything this frame asks of the gpu, cursors included
	gpu_time_begin(&frame->gpu);
	struct flame_pass pass;
	if (!pass_begin(&pass, frame->render, buffer, frame->in_timeline, frame->in_point)) {
		gpu_time_end(&frame->gpu);
		pixman_region32_fini(&repaint);
		wlr_buffer_unlock(buffer);
		return false;
	}

	draw_background(&pass, target, &frame->cur, &repaint, buffer->width, buffer->height);
	for (size_t i = 0; i < frame->cur.len; i++) {
		pass_glass_group(&pass, target, &frame->cur, i);
		draw_element(frame, &pass, target, &frame->cur.data[i], &repaint);
	}
	pixman_region32_fini(&repaint);

	bool submitted = pass_submit(&pass);
	gpu_time_end(&frame->gpu);
	// not submitted: the next frame's point coming covers this one's too
	watch_held(frame);
	if (!submitted) {
		// whatever made it into the buffer can't be trusted
		wlr_damage_ring_add_whole(&frame->ring);
		wlr_buffer_unlock(buffer);
		return false;
	}

	wlr_output_state_set_buffer(state, buffer);
	wlr_buffer_unlock(buffer);
	wlr_output_state_set_damage(state, damage);
	if (frame->in_timeline) {
		// KMS waits for our drawing, then tells us when it let go
		wlr_output_state_set_wait_timeline(state, frame->in_timeline, frame->in_point);
		frame->out_point++;
		wlr_output_state_set_signal_timeline(state, frame->out_timeline, frame->out_point);
	}
	return true;
}

// glass shows what's behind it blurred and bent: anything changing within
// its reach changes all of it. bottom up, one glass changing can reach the
// next. layout coords
static void glass_damage(const struct flame_element_list *list, pixman_region32_t *damage) {
	for (size_t i = 0; i < list->len; i++) {
		const struct flame_element *element = &list->data[i];
		if (element->type != FLAME_ELEMENT_GLASS) {
			continue;
		}
		int grow = elements_glass_reach(element);
		struct wlr_box box = element->clip;
		pixman_box32_t reach = {box.x - grow, box.y - grow, box.x + box.width + grow,
			box.y + box.height + grow};
		if (pixman_region32_contains_rectangle(damage, &reach) != PIXMAN_REGION_OUT) {
			pixman_region32_union_rect(damage, damage, box.x, box.y, box.width, box.height);
		}
	}
}

// what changed since the last frame, buffer coords. the element lists are
// built here too
static void build(struct flame_frame *frame, const struct flame_render_target *target,
		pixman_region32_t *damage) {
	struct wlr_output *output = frame->output;
	struct wlr_box logical = target->logical;

	elements_clear(&frame->cur);
	// the pointer on top of everything, unless it's on the cursor plane
	if (!elements_collect(&frame->cur, &frame->render->scene, logical)
			|| !frame_cursor_build(&frame->cursor, target, &frame->cur)) {
		frame->whole = true; // half a list, don't trust the next diff either
	}
	elements_occlude(&frame->cur);

	// layout coords first: what moved, appeared, vanished, and what clients redrew
	damage_diff(&frame->prev, &frame->cur, damage);
	pixman_region32_union(damage, damage, &frame->pending);
	pixman_region32_clear(&frame->pending);
	glass_damage(&frame->cur, damage);
	pixman_region32_intersect_rect(damage, damage, logical.x, logical.y,
		logical.width, logical.height);

	// then buffer coords
	target_region_to_buffer(target, damage, true);
	if (frame->whole) {
		frame->whole = false;
		wlr_damage_ring_add_whole(&frame->ring);
		pixman_region32_union_rect(damage, damage, 0, 0, output->width, output->height);
	}
	pixman_region32_intersect_rect(damage, damage, 0, 0, output->width, output->height);
}

// a changed frame into state: the candidate's own buffer when KMS takes it,
// else drawn by us. false when neither worked
static bool prepare(struct flame_frame *frame, const struct flame_render_target *target,
		pixman_region32_t *damage, const struct flame_element *candidate,
		struct wlr_output_state *state, enum timing_commit *kind) {
	struct wlr_output *output = frame->output;
	bool scanout = candidate && scanout_try(&frame->scanout, candidate, target, state,
		frame->out_timeline, &frame->out_point);
	bool ready;
	if (scanout) {
		// our buffers didn't see this frame, they catch up when we compose again
		wlr_damage_ring_add(&frame->ring, damage);
		ready = true;
	} else {
		if (frame->scanout.active) {
			// back from scanout: the screen showed a client buffer, ours are old
			wlr_damage_ring_add_whole(&frame->ring);
			pixman_region32_union_rect(damage, damage, 0, 0, output->width, output->height);
		}
		ready = draw(frame, target, damage, state);
	}
	if (scanout != frame->scanout.active) {
		wlr_log(WLR_DEBUG, "%s: direct scanout %s", output->name, scanout ? "on" : "off");
	}
	frame->scanout.active = scanout;
	*kind = scanout ? TIMING_SCANOUT : TIMING_COMPOSITED;
	return ready;
}

static void handle_gpu_done(struct wlr_drm_syncobj_timeline_waiter *waiter) {
	struct flame_frame *frame = wl_container_of(waiter, frame, done_waiter);
	int64_t took = now_ns() - frame->done_since;
	wlr_drm_syncobj_timeline_waiter_finish(&frame->done_waiter);
	frame->done_waiting = false;
	schedule_observe_gpu(&frame->schedule, took);
	timing_gpu_done(&frame->timing, took);
	if (frame->watch.on && took > frame->watch.worst_gpu) {
		frame->watch.worst_gpu = took;
	}
}

// a frame's drawing just went to the gpu: when does it finish. every drawn
// frame, sent to KMS now or held as the successor: measuring only the ones
// sent at once would stop measuring the moment the lead grows enough to
// render ahead, and the lead would never come down again
static void watch_gpu_done(struct flame_frame *frame) {
	if (!frame->in_timeline || frame->done_waiting) {
		return;
	}
	frame->done_since = now_ns();
	frame->done_waiting = wlr_drm_syncobj_timeline_waiter_init(&frame->done_waiter,
		frame->in_timeline, frame->in_point, 0, frame->output->event_loop, handle_gpu_done);
}

// a cursor commit sends the buffer on screen again. when that's a client's
// (direct scanout), KMS letting go of the previous commit no longer means
// the client may have it back: this commit holds it too. without that, the
// client reuses it while it's still on screen (a video player decodes the
// next frame into it, black blocks whenever the mouse moves)
static void keep_scanout_buffer(struct flame_frame *frame,
		const struct flame_element *candidate, struct wlr_output_state *state) {
	if (!frame->scanout.active || !candidate || !candidate->surface || !candidate->buffer
			|| !frame->out_timeline) {
		return;
	}
	struct wlr_buffer *buffer = candidate->buffer;
	struct wlr_client_buffer *client = wlr_client_buffer_get(buffer);
	if (client && client->source) {
		buffer = client->source;
	}
	if (buffer != frame->front) {
		return;
	}
	struct wlr_linux_drm_syncobj_surface_v1_state *sync =
		wlr_linux_drm_syncobj_v1_get_surface_state(candidate->surface);
	if (!sync) {
		return; // implicit sync: the backend holds the buffer while it shows
	}
	frame->out_point++;
	wlr_output_state_set_signal_timeline(state, frame->out_timeline, frame->out_point);
	wlr_linux_drm_syncobj_v1_state_add_release_point(sync, frame->out_timeline,
		frame->out_point, frame->output->event_loop);
}

// the app that has the output to itself: the topmost thing showing on all of
// area (a fullscreen game or video), drawn or scanned out, a software
// pointer over it or not. NULL for none
static const struct flame_element *fullscreen_element(const struct flame_element_list *list,
		struct wlr_box area) {
	pixman_box32_t box = {area.x, area.y, area.x + area.width, area.y + area.height};
	for (size_t i = list->len; i-- > 0;) {
		const struct flame_element *element = &list->data[i];
		if (element->type == FLAME_ELEMENT_RECT) {
			continue; // a backdrop, not an app
		}
		if (pixman_region32_contains_rectangle((pixman_region32_t *)&element->visible, &box)
				== PIXMAN_REGION_IN) {
			return element;
		}
	}
	return NULL;
}

// it asked for its frames to go out mid refresh: a wayland app through
// tearing control, an X one with PresentOptionAsync
static bool app_wants_tearing(struct flame_frame *frame, const struct flame_element *element) {
	if (!element) {
		return false;
	}
	if (element->type == FLAME_ELEMENT_SURFACE && element->surface) {
		return wlr_tearing_control_manager_v1_surface_hint_from_surface(frame->render->tearing,
			element->surface) == WP_TEARING_CONTROL_V1_PRESENTATION_HINT_ASYNC;
	}
	return element->buffer_node && element->buffer_node->tearing;
}

static void vrr_refused(struct flame_frame *frame) {
	sync_vrr_refused(&frame->sync);
	if (!frame->vrr_logged) {
		wlr_log(WLR_INFO, "%s: adaptive sync won't switch, it stays %s until the next mode "
			"change or vt switch", frame->output->name,
			frame->output->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED
				? "on" : "off");
		frame->vrr_logged = true;
	}
}

// vrr switches with this frame if KMS takes it: a test commit first, either
// way (nested backends can't turn it off), so a display that won't never
// costs a frame
static void vrr_set(struct flame_frame *frame, struct wlr_output_state *state, bool on) {
	wlr_output_state_set_adaptive_sync_enabled(state, on);
	if (!wlr_output_test_state(frame->output, state)) {
		state->committed &= ~WLR_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED;
		vrr_refused(frame);
	}
}

// a picture's own buffer goes to KMS (a fullscreen X game): a draw id for
// it, and its owner holds on to the buffer until a later frame flips
static uint64_t picture_scanned_out(struct flame_frame *frame,
		const struct flame_element *candidate) {
	struct flame_buffer_node *node = candidate->buffer_node;
	frame->draw_id++;
	if (node->impl && node->impl->scanned_out) {
		node->impl->scanned_out(node, frame, frame->draw_id);
	}
	return frame->draw_id;
}

// the schedule said now: build a frame from the latest scene and send it to
// KMS, or hold it behind the pending one. nothing changed: no pageflip at all
static void render(struct flame_frame *frame, int64_t now) {
	struct wlr_output *output = frame->output;
	struct schedule *schedule = &frame->schedule;
	bool again = false;
	if (frame->hooks.before_render) {
		// the refresh this frame is going for: after the pending one, if any
		int64_t target = schedule_next_vblank(schedule, now);
		if (schedule->pending) {
			target = schedule_next_vblank(schedule, target);
		}
		again = frame->hooks.before_render(frame->hooks.data, target);
	}

	frame->timing.pace_fps = frame->pace_fps;
	struct flame_render_target target;
	target_init(&target, output, *frame->area);
	pixman_region32_t damage;
	pixman_region32_init(&damage);
	build(frame, &target, &damage);
	// the diff in build needed the old frame_seq, from here on this is the frame
	feedback_drawn(&frame->render->surfaces, output, &frame->cur,
		frame_cursor_surface(&frame->cursor));

	// wlroots wanting a real commit (screencopy) counts as a change
	bool changed = pixman_region32_not_empty(&damage) || output->needs_frame;
	// only the cursor plane changed: the buffer on screen goes again, the new
	// plane state rides along. no drawing, no gpu
	bool cursor_only = !changed && frame_cursor_dirty(&frame->cursor);
	if (cursor_only && !frame->front) {
		changed = true; // nothing to send again yet
		cursor_only = false;
	}
	// vrr: the display waits for frames where it can and init.lua wants it.
	// the switch rides on a frame, a still screen gets one for it
	const struct flame_config *config = frame->render->server->config;
	bool fullscreen = frame->hooks.fullscreen && frame->hooks.fullscreen(frame->hooks.data);
	bool vrr_want = sync_vrr_wanted(&frame->sync, config->vrr, output->adaptive_sync_supported,
		fullscreen, now);
	bool vrr_change = sync_vrr_switch(&frame->sync, vrr_want,
		output->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED);
	if (vrr_change && !changed) {
		changed = true;
		cursor_only = false;
	}
	bool submitted_now = false;
	bool drew_gl = false; // went through the gpu, not straight to KMS
	const struct flame_element *candidate = scanout_candidate(&frame->cur, *frame->area);
	scanout_hint(&frame->scanout, candidate);
	// the fullscreen app wants to tear (vsync off): its frames go out the
	// moment they're ready, as async flips where KMS takes them
	bool tearing = config->allow_tearing && !frame->sync.tearing_off
		&& app_wants_tearing(frame, fullscreen_element(&frame->cur, *frame->area));
	frame->sync.tearing = tearing;
	bool drew = false;
	bool failed = false; // it tried and didn't make it: it's asked for again
	if (changed || cursor_only) {
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		enum timing_commit kind = TIMING_CURSOR;
		bool ready = true;
		// what buffer nodes will hear about this frame, nothing new for a
		// cursor move or a client buffer scanned out
		uint64_t draw_id = 0;
		if (cursor_only) {
			// no damage set: it's the same picture either way
			wlr_output_state_set_buffer(&state, frame->front);
			keep_scanout_buffer(frame, candidate, &state);
			if (frame->scanout.active && candidate && candidate->buffer_node
					&& candidate->buffer == frame->front) {
				draw_id = picture_scanned_out(frame, candidate); // on screen again
			}
		} else {
			ready = prepare(frame, &target, &damage, candidate, &state, &kind);
			drew_gl = kind == TIMING_COMPOSITED;
			if (drew_gl && ready && frame->watch.on) {
				frame->watch.composited++;
			}
			if (drew_gl && ready) {
				watch_gpu_done(frame);
			}
			if (drew_gl) {
				draw_id = frame->draw_id;
			} else if (ready && candidate->buffer_node) {
				draw_id = picture_scanned_out(frame, candidate);
			}
		}

		int64_t vrr_since = 0;
		if (ready && vrr_change) {
			vrr_since = now_ns();
			vrr_set(frame, &state, vrr_want);
		}
		if (!ready) {
			wlr_output_state_finish(&state);
			frame->whole = true;
			failed = true;
		} else if (!schedule->pending) {
			// a cursor plane change can't ride along an async flip
			// nor a vrr switch: KMS refuses an async flip that changes it
			bool tear = !cursor_only && !(state.committed & WLR_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED)
				&& sync_tearing_wanted(&frame->sync, config->allow_tearing, tearing,
					frame_cursor_dirty(&frame->cursor));
			bool torn = false;
			drew = commit(frame, &state, kind, tear, &torn);
			bool vrr_asked = state.committed & WLR_OUTPUT_STATE_ADAPTIVE_SYNC_ENABLED;
			wlr_output_state_finish(&state);
			if (drew && vrr_asked) {
				bool on = output->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED;
				if (vrr_want != on) {
					vrr_refused(frame); // took the commit, didn't switch
				} else {
					wlr_log(WLR_INFO, "%s: adaptive sync %s, test and commit took %.1f ms",
						output->name, on ? "on" : "off", (now_ns() - vrr_since) / 1e6);
					frame->vrr_switch_since = vrr_since;
				}
			}
			if (drew) {
				submitted_now = true;
				frame->pending_draw = draw_id;
				schedule_submitted(schedule, &frame->submitted_ticket, now, now_ns());
				if (torn) {
					schedule_mark_off_grid(schedule);
				}
				if (schedule->pending_target) {
					timing_slack(&frame->timing,
						schedule->pending_target - schedule->pending_submit);
				}
			} else {
				frame->whole = true;
				failed = true;
			}
		} else {
			// behind the pending flip, it goes in the moment that one lands
			frame->queued = state;
			frame->has_queued = true;
			frame->queued_kind = kind;
			frame->queued_draw = draw_id;
			schedule_queued(schedule, &frame->queued_ticket);
			timing_successor_prepared(&frame->timing);
			drew = true;
		}
	}
	pixman_region32_fini(&damage);
	if (!drew) {
		// nothing went anywhere: callbacks on the refresh grid, no pageflip
		schedule_empty(schedule, now);
		if (failed) {
			// KMS refused it or it couldn't be drawn (no buffer free): the
			// screen still shows the old picture, the next refresh tries again
			schedule_request_redraw(schedule);
		}
	}
	// a game tearing, or the display following the frames (vrr): the next
	// frame goes the moment it's ready too
	schedule_set_asap(schedule, tearing
		|| output->adaptive_sync_status == WLR_OUTPUT_ADAPTIVE_SYNC_ENABLED);
	swap_lists(frame);
	if (again) {
		// still animating: the refresh after this one too
		schedule_request_redraw(schedule);
	}
	if (submitted_now) {
		// apps that can't draw a frame between the flip and the deadline get
		// the next cycle's turn now: they keep their frame rate, everyone
		// else waits for the flip and makes the next refresh
		// the lead has the margin in it already
		int64_t budget = schedule->period - schedule_lead(schedule);
		budget = budget > 0 ? budget : 0;
		feedback_frame_done_early(&frame->prev, frame->callback_cycle + 1, now_ns(), budget);
	}

	int64_t end = now_ns();
	// a cursor commit costs nothing, it would only drag the estimate down.
	// a driver stall on its rhythm stays out too: it's planned for on its
	// own, in the estimate it would slow every frame after it
	bool composed = drew && !cursor_only;
	bool stall = false;
	if (drew_gl) {
		int64_t planned = schedule->extra;
		stall = hiccup_draw(&frame->hiccup, end - now);
		if (stall) {
			timing_hiccup(&frame->timing, planned > 0);
		}
		schedule_set_extra(schedule, hiccup_next(&frame->hiccup));
	}
	if (composed && !stall) {
		schedule_observe_cpu(schedule, end - now);
	}
	struct timespec start_ts = ns_timespec(now), end_ts = ns_timespec(end);
	timing_frame(&frame->timing, &start_ts, &end_ts, composed);
}

static void send_callbacks(struct flame_frame *frame, int64_t now) {
	struct timespec when = ns_timespec(now);
	bool new_cycle = schedule_callbacks_new_cycle(&frame->schedule, now);
	if (new_cycle) {
		frame->callback_cycle++;
	}
	// prev is the last frame built, the one that just went out. apps that
	// draw fast get woken just in time for the render that can show them
	frame->jit_wake = feedback_frame_done(&frame->prev, frame_cursor_surface(&frame->cursor),
		frame->callback_cycle, now, schedule_render_at(&frame->schedule, now));
	schedule_callbacks_sent(&frame->schedule, now);
	if (frame->hooks.callbacks_sent) {
		frame->hooks.callbacks_sent(frame->hooks.data, &when, new_cycle);
	}
}

static void process(struct flame_frame *frame) {
	frame->processing = true;
	process_once(frame);
	frame->processing = false;
}

static void process_once(struct flame_frame *frame) {
	struct schedule *schedule = &frame->schedule;
	struct wlr_output *output = frame->output;
	int64_t now = now_ns();

	// earlier frames' gpu times, whatever finished by now
	double gpu_ms;
	if (gpu_time_collect(&frame->gpu, &gpu_ms) > 0) {
		if (!frame->in_timeline) {
			schedule_observe_gpu(schedule, (int64_t)(gpu_ms * 1e6));
		}
		timing_gpu(&frame->timing, gpu_ms);
		if (frame->watch.on && gpu_ms > frame->watch.worst_own_ms) {
			frame->watch.worst_own_ms = gpu_ms;
		}
	}

	if (!output->enabled || wlr_box_empty(frame->area)) {
		timer_arm(&frame->wake, 0);
		return; // comes back through the commit that enables it
	}
	if (schedule_stalled(schedule, now)) {
		// a flip that never reported back (a connector going away mid flip,
		// someone else's commit): it's taken as done. one held back behind it
		// would wait for it forever: it goes, and the screen's drawn again
		wlr_log(WLR_ERROR, "%s: a pageflip has been pending for over %llds, drawing again",
			output->name, SCHEDULE_WATCHDOG_NS / 1000000000);
		schedule_presented(schedule, 0, now);
		if (frame->has_queued) {
			drop_queued(frame);
			schedule_discard_queued(schedule);
			timing_successor(&frame->timing, false);
		}
		frame->whole = true;
		schedule_request_redraw(schedule);
	}
	// vrr's debounce settled with nothing else changing: a frame for the switch
	int64_t settle = sync_vrr_settles_at(&frame->sync);
	if (settle && now >= settle) {
		schedule_request_redraw(schedule);
	}
	if (schedule_render_due(schedule, now)) {
		render(frame, now);
		now = now_ns();
	}
	if (schedule_callbacks_due(schedule, now)) {
		send_callbacks(frame, now);
	} else if (frame->jit_wake && now >= frame->jit_wake) {
		// the ones held back for just in time
		frame->jit_wake = feedback_frame_done(&frame->prev,
			frame_cursor_surface(&frame->cursor), frame->callback_cycle, now,
			schedule_render_at(schedule, now));
	}

	// asked again after its turn: a flip would start the next cycle, without
	// one the refresh grid does
	if (feedback_waiting_next_cycle(&frame->prev, frame->callback_cycle)) {
		schedule_callbacks_next(schedule, now);
	}
	// covered windows still asking for callbacks, a few times a second
	int64_t background = feedback_background(&frame->prev, frame->callback_cycle, now);

	// sleep until the next thing is due, never spin on something due already
	int64_t deadline = schedule_deadline(schedule, now);
	if (deadline > 0 && deadline <= now) {
		deadline = now + schedule->period;
	}
	if (background && (!deadline || background < deadline)) {
		deadline = background;
	}
	if (frame->jit_wake && (!deadline || frame->jit_wake < deadline)) {
		deadline = frame->jit_wake > now ? frame->jit_wake : now + 1;
	}
	// a debounce still to settle wakes us for it. one settled already asked
	// for its frame above, the schedule's deadline has it
	settle = sync_vrr_settles_at(&frame->sync);
	if (settle > now && (!deadline || settle < deadline)) {
		deadline = settle;
	}
	timer_arm(&frame->wake, deadline);
}
