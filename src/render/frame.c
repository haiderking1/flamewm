#include "render/frame.h"

#include <wlr/backend.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_output.h>
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

// ---- what wakes us up ----

static void handle_scene_changed(struct wl_listener *listener, void *data) {
	struct flame_frame *frame = wl_container_of(listener, frame, scene_changed);
	wlr_output_schedule_frame(frame->output);
}

static void handle_scene_damage(struct wl_listener *listener, void *data) {
	struct flame_frame *frame = wl_container_of(listener, frame, scene_damage);
	const pixman_region32_t *damage = data;
	pixman_region32_union(&frame->pending, &frame->pending, damage);
	// scene_changed follows and schedules the frame
}

// software cursors moving, wlroots tells us where
static void handle_output_damage(struct wl_listener *listener, void *data) {
	struct flame_frame *frame = wl_container_of(listener, frame, output_damage);
	const struct wlr_output_event_damage *event = data;
	struct wlr_output *output = frame->output;

	int width, height;
	wlr_output_transformed_resolution(output, &width, &height);
	pixman_region32_t damage;
	pixman_region32_init(&damage);
	wlr_region_transform(&damage, event->damage, wlr_output_transform_invert(output->transform),
		width, height);
	pixman_region32_union(&frame->pending_buffer, &frame->pending_buffer, &damage);
	pixman_region32_fini(&damage);
	wlr_output_schedule_frame(output);
}

static void handle_output_needs_frame(struct wl_listener *listener, void *data) {
	struct flame_frame *frame = wl_container_of(listener, frame, output_needs_frame);
	wlr_output_schedule_frame(frame->output);
}

static void handle_output_commit(struct wl_listener *listener, void *data) {
	struct flame_frame *frame = wl_container_of(listener, frame, output_commit);
	const struct wlr_output_event_commit *event = data;
	// a different size or orientation leaves nothing in the old buffers usable
	if (event->state->committed & (WLR_OUTPUT_STATE_MODE | WLR_OUTPUT_STATE_SCALE
			| WLR_OUTPUT_STATE_TRANSFORM | WLR_OUTPUT_STATE_ENABLED)) {
		frame->whole = true;
		wlr_output_schedule_frame(frame->output);
	}
}

// ---- setup ----

bool frame_init(struct flame_frame *frame, struct flame_render *render,
		struct wlr_output *output) {
	frame->output = output;
	frame->render = render;
	frame->whole = true;
	wlr_damage_ring_init(&frame->ring);
	pixman_region32_init(&frame->pending);
	pixman_region32_init(&frame->pending_buffer);
	elements_init(&frame->prev);
	elements_init(&frame->cur);

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

	listener_connect(&frame->scene_changed, &render->scene.events.changed,
		handle_scene_changed);
	listener_connect(&frame->scene_damage, &render->scene.events.damage, handle_scene_damage);
	listener_connect(&frame->output_damage, &output->events.damage, handle_output_damage);
	listener_connect(&frame->output_needs_frame, &output->events.needs_frame,
		handle_output_needs_frame);
	listener_connect(&frame->output_commit, &output->events.commit, handle_output_commit);
	return true;
}

void frame_finish(struct flame_frame *frame) {
	if (frame->scene_changed.link.next) {
		listener_disconnect(&frame->scene_changed);
		listener_disconnect(&frame->scene_damage);
		listener_disconnect(&frame->output_damage);
		listener_disconnect(&frame->output_needs_frame);
		listener_disconnect(&frame->output_commit);
	}
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
	pixman_region32_fini(&frame->pending_buffer);
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

	struct wlr_linux_drm_syncobj_surface_v1_state *sync =
		wlr_linux_drm_syncobj_v1_get_surface_state(element->surface);
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
	sampled(frame, element->surface);
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

static bool draw_and_commit(struct flame_frame *frame,
		const struct flame_render_target *target, const pixman_region32_t *damage) {
	struct wlr_output *output = frame->output;
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	bool ok = false;

	if (!wlr_output_configure_primary_swapchain(output, &state, &output->swapchain)) {
		wlr_log(WLR_ERROR, "no swapchain for %s", output->name);
		goto out;
	}
	struct wlr_buffer *buffer = wlr_swapchain_acquire(output->swapchain);
	if (!buffer) {
		goto out;
	}

	// this frame's damage, grown by what this buffer missed since it was last used
	pixman_region32_t repaint;
	pixman_region32_init(&repaint);
	wlr_damage_ring_add(&frame->ring, damage);
	wlr_damage_ring_rotate_buffer(&frame->ring, buffer, &repaint);

	frame->in_point++;
	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(frame->render->renderer,
		buffer, &(struct wlr_buffer_pass_options){
			.signal_timeline = frame->in_timeline,
			.signal_point = frame->in_point,
		});
	if (!pass) {
		pixman_region32_fini(&repaint);
		wlr_buffer_unlock(buffer);
		goto out;
	}

	draw_background(pass, target, &frame->cur, &repaint, buffer->width, buffer->height);
	for (size_t i = 0; i < frame->cur.len; i++) {
		draw_element(frame, pass, target, &frame->cur.data[i], &repaint);
	}
	wlr_output_add_software_cursors_to_render_pass(output, pass, &repaint);
	pixman_region32_fini(&repaint);

	if (!wlr_render_pass_submit(pass)) {
		// whatever made it into the buffer can't be trusted
		wlr_damage_ring_add_whole(&frame->ring);
		wlr_buffer_unlock(buffer);
		goto out;
	}

	wlr_output_state_set_buffer(&state, buffer);
	wlr_buffer_unlock(buffer);
	wlr_output_state_set_damage(&state, damage);
	if (frame->in_timeline) {
		// KMS waits for our drawing, then tells us when it let go
		wlr_output_state_set_wait_timeline(&state, frame->in_timeline, frame->in_point);
		frame->out_point++;
		wlr_output_state_set_signal_timeline(&state, frame->out_timeline, frame->out_point);
	}

	ok = wlr_output_commit_state(output, &state);
	if (!ok) {
		wlr_log(WLR_DEBUG, "commit on %s failed", output->name);
	}
out:
	wlr_output_state_finish(&state);
	return ok;
}

void frame_render(struct flame_frame *frame, struct wlr_box logical,
		const struct timespec *now) {
	struct wlr_output *output = frame->output;
	struct flame_render *render = frame->render;
	if (!output->enabled || wlr_box_empty(&logical)) {
		return;
	}

	struct flame_render_target target;
	target_init(&target, output, logical);

	elements_clear(&frame->cur);
	if (!elements_collect(&frame->cur, &render->scene, logical)) {
		frame->whole = true; // half a list, don't trust the next diff either
	}
	elements_occlude(&frame->cur);

	// layout coords first: what moved, appeared, vanished, and what clients redrew
	pixman_region32_t damage;
	pixman_region32_init(&damage);
	damage_diff(&frame->prev, &frame->cur, &damage);
	pixman_region32_union(&damage, &damage, &frame->pending);
	pixman_region32_clear(&frame->pending);
	pixman_region32_intersect_rect(&damage, &damage, logical.x, logical.y,
		logical.width, logical.height);

	// then buffer coords, plus what wlroots told us about (cursors)
	target_region_to_buffer(&target, &damage, true);
	pixman_region32_union(&damage, &damage, &frame->pending_buffer);
	pixman_region32_clear(&frame->pending_buffer);
	if (frame->whole) {
		frame->whole = false;
		wlr_damage_ring_add_whole(&frame->ring);
		pixman_region32_union_rect(&damage, &damage, 0, 0, output->width, output->height);
	}
	pixman_region32_intersect_rect(&damage, &damage, 0, 0, output->width, output->height);

	// the diff above needed the old frame_seq, from here on this is the frame
	feedback_drawn(&render->surfaces, output, &frame->cur);

	// wlroots asks for a real commit when something waits on one: frame
	// callbacks with no new pixels, screencopy. skipping it would mean the
	// next frame comes from an idle callback right away instead of the next
	// vblank, and that spins the event loop so hard timers and signals never run
	if (pixman_region32_not_empty(&damage) || output->needs_frame) {
		draw_and_commit(frame, &target, &damage);
	}
	pixman_region32_fini(&damage);

	feedback_frame_done(&frame->cur, now);
	swap_lists(frame);
}
