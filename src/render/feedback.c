#include "render/feedback.h"

#include <math.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_output.h>

#include "render/elements.h"
#include "scene/scene.h"
#include "scene/surface.h"

static void enter(struct flame_surface_state *state, struct wlr_output *output) {
	if (state->entered == output) {
		return;
	}
	if (state->entered) {
		wlr_surface_send_leave(state->surface, state->entered);
	}
	state->entered = output;
	wlr_surface_send_enter(state->surface, output);
	// clients render at the output's scale instead of getting stretched
	wlr_fractional_scale_v1_notify_scale(state->surface, output->scale);
	wlr_surface_set_preferred_buffer_scale(state->surface, (int32_t)ceil(output->scale));
}

void feedback_drawn(struct flame_scene_surfaces *surfaces, struct wlr_output *output,
		const struct flame_element_list *list, struct wlr_surface *plane) {
	struct flame_scene *scene = surfaces->scene;
	uint64_t seq = ++scene->frame_seq;

	for (size_t i = 0; i < list->len; i++) {
		const struct flame_element *element = &list->data[i];
		if (element->type != FLAME_ELEMENT_SURFACE) {
			continue;
		}
		struct flame_surface_state *state = scene_surface_state(element->surface);
		if (!state) {
			continue;
		}
		state->drawn_seq = seq;
		state->drawn_x = element->box.x;
		state->drawn_y = element->box.y;
		const pixman_box32_t *shows = pixman_region32_extents(&element->visible);
		state->drawn_visible = pixman_region32_not_empty(&element->visible)
			? (struct wlr_box){shows->x1, shows->y1, shows->x2 - shows->x1, shows->y2 - shows->y1}
			: (struct wlr_box){0};
		enter(state, output);
	}
	struct flame_surface_state *plane_state = plane ? scene_surface_state(plane) : NULL;
	if (plane_state) {
		// shows, but nothing we draw: its commits repaint nothing
		plane_state->drawn_seq = seq;
		plane_state->drawn_visible = (struct wlr_box){0};
		enter(plane_state, output);
	}

	// went off screen: hidden workspace, unmapped, moved away
	struct flame_surface_state *state;
	wl_list_for_each(state, &surfaces->states, link) {
		if (state->entered == output && state->drawn_seq != seq) {
			wlr_surface_send_leave(state->surface, output);
			state->entered = NULL;
		}
	}
}

#define BACKGROUND_INTERVAL_NS 250000000LL // four times a second

static struct timespec timespec_from_ns(int64_t ns) {
	return (struct timespec){.tv_sec = ns / 1000000000, .tv_nsec = ns % 1000000000};
}

static void send(struct flame_surface_state *state, uint64_t cycle, int64_t now) {
	struct timespec when = timespec_from_ns(now);
	wlr_surface_send_frame_done(state->surface, &when);
	state->callback_cycle = cycle;
	state->callback_time = now;
}

void feedback_frame_done(const struct flame_element_list *list, struct wlr_surface *plane,
		uint64_t cycle, int64_t now) {
	struct flame_surface_state *plane_state = plane ? scene_surface_state(plane) : NULL;
	if (plane_state && plane_state->callback_cycle != cycle) {
		send(plane_state, cycle, now);
	}
	for (size_t i = 0; i < list->len; i++) {
		const struct flame_element *element = &list->data[i];
		if (element->type != FLAME_ELEMENT_SURFACE
				|| !pixman_region32_not_empty(&element->visible)) {
			continue;
		}
		struct flame_surface_state *state = scene_surface_state(element->surface);
		if (!state) {
			// untracked, can't tell if it had its turn: let it have one
			struct timespec when = timespec_from_ns(now);
			wlr_surface_send_frame_done(element->surface, &when);
		} else if (state->callback_cycle != cycle) {
			// its turn this cycle, even with nothing asked for yet: asking
			// later waits for the next cycle, nobody wakes twice a refresh
			send(state, cycle, now);
		}
	}
}

int64_t feedback_background(const struct flame_element_list *list, uint64_t cycle, int64_t now) {
	int64_t next = 0;
	for (size_t i = 0; i < list->len; i++) {
		const struct flame_element *element = &list->data[i];
		if (element->type != FLAME_ELEMENT_SURFACE
				|| pixman_region32_not_empty(&element->visible)
				|| wl_list_empty(&element->surface->current.frame_callback_list)) {
			continue;
		}
		struct flame_surface_state *state = scene_surface_state(element->surface);
		if (!state) {
			continue;
		}
		// the first request counts from when we first saw it waiting, so a
		// surface that never showed doesn't get it right away or never
		if (!state->callback_time) {
			state->callback_time = now;
		}
		int64_t due = state->callback_time + BACKGROUND_INTERVAL_NS;
		if (now >= due) {
			// asking again re-arms it from there
			send(state, cycle, now);
		} else if (!next || due < next) {
			next = due;
		}
	}
	return next;
}
