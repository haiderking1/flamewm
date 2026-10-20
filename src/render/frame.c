#include "render/frame.h"

#include <wlr/backend.h>
#include <wlr/backend/session.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>
#include <wlr/util/transform.h>

#include "render/coords.h"
#include "render/damage.h"
#include "render/feedback.h"
#include "render/render.h"
#include "scene/scene.h"
#include "util/listener.h"

static void process(struct flame_frame *frame);
static void process_once(struct flame_frame *frame);

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
		schedule_submitted(&frame->schedule, &frame->foreign_ticket, now_ns());
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
		frame_request(frame);
	}
}

static void handle_output_present(struct wl_listener *listener, void *data) {
	struct flame_frame *frame = wl_container_of(listener, frame, output_present);
	const struct wlr_output_event_present *event = data;
	// a flip without a timestamp anchors on dispatch time instead
	frame->present_ns = event->presented ? timespec_ns(&event->when) : 0;
}

static bool commit(struct flame_frame *frame, struct wlr_output_state *state,
		enum timing_commit kind) {
	frame->committing = true;
	bool ok = wlr_output_commit_state(frame->output, state);
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
	frame->present_ns = 0;
	frame->stall_reported = false;

	if (frame->has_queued) {
		bool ok = commit(frame, &frame->queued, frame->queued_kind);
		drop_queued(frame);
		if (ok) {
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
	frame->stall_reported = false;
	frame->scanout.suspended = false;
	frame_cursor_reset(&frame->cursor);
	queue_process(frame);
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
	wlr_damage_ring_init(&frame->ring);
	pixman_region32_init(&frame->pending);
	elements_init(&frame->prev);
	elements_init(&frame->cur);
	schedule_init(&frame->schedule, render->pipeline, refresh_period(output));
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

static void draw_element(struct flame_frame *frame, struct wlr_render_pass *pass,
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

	struct wlr_box dst = element->box;
	target_box_to_buffer(target, &dst);

	if (element->type == FLAME_ELEMENT_RECT) {
		wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
			.box = dst,
			.color = {
				.r = element->color[0],
				.g = element->color[1],
				.b = element->color[2],
				.a = element->color[3],
			},
			.clip = &region,
		});
		pixman_region32_fini(&region);
		return;
	}

	// fully opaque where we draw: no blending needed
	pixman_region32_t translucent;
	pixman_region32_init(&translucent);
	pixman_region32_copy(&translucent, &element->opaque);
	target_region_to_buffer(target, &translucent, false);
	pixman_region32_subtract(&translucent, &region, &translucent);
	bool blend = pixman_region32_not_empty(&translucent);
	pixman_region32_fini(&translucent);

	// ours (the pointer) has nothing to wait for and nobody to give it back to
	bool client = element->type == FLAME_ELEMENT_SURFACE;
	struct wlr_linux_drm_syncobj_surface_v1_state *sync =
		client ? wlr_linux_drm_syncobj_v1_get_surface_state(element->surface) : NULL;
	enum wl_output_transform transform = wlr_output_transform_compose(
		wlr_output_transform_invert(element->transform), target->transform);
	wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
		.texture = element->texture,
		.src_box = element->src,
		.dst_box = dst,
		.transform = transform,
		.clip = &region,
		.blend_mode = blend ? WLR_RENDER_BLEND_MODE_PREMULTIPLIED : WLR_RENDER_BLEND_MODE_NONE,
		.wait_timeline = sync ? sync->acquire_timeline : NULL,
		.wait_point = sync ? sync->acquire_point : 0,
	});
	if (client) {
		sampled(frame, element->surface);
	}
	pixman_region32_fini(&region);
}

// black where nothing opaque covers the repaint area
static void draw_background(struct wlr_render_pass *pass,
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
	wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
		.box = {.width = width, .height = height},
		.color = {.r = 0, .g = 0, .b = 0, .a = 1},
		.clip = &background,
	});
	pixman_region32_fini(&background);
}

// renders the frame into a buffer and fills state with it, ready to commit
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

	frame->in_point++;
	// everything this frame asks of the gpu, cursors included
	gpu_time_begin(&frame->gpu);
	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(frame->render->renderer,
		buffer, &(struct wlr_buffer_pass_options){
			.signal_timeline = frame->in_timeline,
			.signal_point = frame->in_point,
		});
	if (!pass) {
		gpu_time_end(&frame->gpu);
		pixman_region32_fini(&repaint);
		wlr_buffer_unlock(buffer);
		return false;
	}

	draw_background(pass, target, &frame->cur, &repaint, buffer->width, buffer->height);
	for (size_t i = 0; i < frame->cur.len; i++) {
		draw_element(frame, pass, target, &frame->cur.data[i], &repaint);
	}
	pixman_region32_fini(&repaint);

	bool submitted = wlr_render_pass_submit(pass);
	gpu_time_end(&frame->gpu);
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

// the schedule said now: build a frame from the latest scene and send it to
// KMS, or hold it behind the pending one. nothing changed: no pageflip at all
static void render(struct flame_frame *frame, int64_t now) {
	struct wlr_output *output = frame->output;
	struct schedule *schedule = &frame->schedule;
	if (frame->hooks.before_render) {
		frame->hooks.before_render(frame->hooks.data);
	}

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
	const struct flame_element *candidate = scanout_candidate(&frame->cur, *frame->area);
	scanout_hint(&frame->scanout, candidate);
	bool drew = false;
	if (changed || cursor_only) {
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		enum timing_commit kind = TIMING_CURSOR;
		bool ready = true;
		if (cursor_only) {
			// no damage set: it's the same picture either way
			wlr_output_state_set_buffer(&state, frame->front);
		} else {
			ready = prepare(frame, &target, &damage, candidate, &state, &kind);
		}

		if (!ready) {
			wlr_output_state_finish(&state);
			frame->whole = true;
		} else if (!schedule->pending) {
			drew = commit(frame, &state, kind);
			wlr_output_state_finish(&state);
			if (drew) {
				schedule_submitted(schedule, &frame->submitted_ticket, now_ns());
			} else {
				frame->whole = true;
			}
		} else {
			// behind the pending flip, it goes in the moment that one lands
			frame->queued = state;
			frame->has_queued = true;
			frame->queued_kind = kind;
			schedule_queued(schedule, &frame->queued_ticket);
			timing_successor_prepared(&frame->timing);
			drew = true;
		}
	}
	pixman_region32_fini(&damage);
	if (!drew) {
		// nothing went anywhere: callbacks on the refresh grid, no pageflip
		schedule_empty(schedule, now);
	}
	swap_lists(frame);

	int64_t end = now_ns();
	// a cursor commit costs nothing, it would only drag the estimate down
	bool composed = drew && !cursor_only;
	if (composed) {
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
	// prev is the last frame built, the one that just went out
	feedback_frame_done(&frame->prev, frame_cursor_surface(&frame->cursor),
		frame->callback_cycle, now);
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
		schedule_observe_gpu(schedule, (int64_t)(gpu_ms * 1e6));
		timing_gpu(&frame->timing, gpu_ms);
	}

	if (!output->enabled || wlr_box_empty(frame->area)) {
		timer_arm(&frame->wake, 0);
		return; // comes back through the commit that enables it
	}
	if (schedule_stalled(schedule, now) && schedule->pending == &frame->foreign_ticket) {
		// that flip never reported back, nothing of ours is waiting on it
		schedule_presented(schedule, 0, now);
	}
	if (schedule_stalled(schedule, now) && !frame->stall_reported) {
		wlr_log(WLR_ERROR, "%s: a pageflip has been pending for over %llds",
			output->name, SCHEDULE_WATCHDOG_NS / 1000000000);
		frame->stall_reported = true;
	}
	if (schedule_render_due(schedule, now)) {
		render(frame, now);
		now = now_ns();
	}
	if (schedule_callbacks_due(schedule, now)) {
		send_callbacks(frame, now);
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
	timer_arm(&frame->wake, deadline);
}
