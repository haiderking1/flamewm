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
	// cycles are counted per output, from 1 for a new one (one coming back
	// after a vt switch is new): the old output's count means nothing here.
	// kept, a surface would get no callbacks until this one caught up with
	// it, frozen for as long as the old output ran
	state->callback_cycle = 0;
	state->jit_wake = 0;
	state->jit_target = 0;
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
	// only a callback it asked for times its drawing
	state->drawing = !wl_list_empty(&state->surface->current.frame_callback_list);
	wlr_surface_send_frame_done(state->surface, &when);
	state->answered_at = now;
	state->callback_cycle = cycle;
	state->callback_time = now;
	state->jit_target = 0; // the just in time path sets it after
	state->jit_wake = 0;
}


void feedback_frame_done_early(const struct flame_element_list *list, uint64_t next_cycle,
		int64_t now, int64_t budget) {
	for (size_t i = 0; i < list->len; i++) {
		const struct flame_element *element = &list->data[i];
		if (element->type != FLAME_ELEMENT_SURFACE
				|| !pixman_region32_not_empty(&element->visible)) {
			continue;
		}
		struct flame_surface_state *state = scene_surface_state(element->surface);
		if (state && state->draw_time > budget && state->callback_cycle < next_cycle) {
			send(state, next_cycle, now);
		}
	}
}

#define JIT_MIN_MARGIN_NS 1000000LL

// when to wake a surface so its frame lands right before render_at: its draw
// time and some room for it to vary. 0: now (unknown draw time, no target)
static int64_t wake_time(const struct flame_surface_state *state, int64_t render_at) {
	if (!render_at || state->draw_time <= 0
			|| wl_list_empty(&state->surface->current.frame_callback_list)) {
		return 0;
	}
	int64_t margin = state->draw_time / 2 > JIT_MIN_MARGIN_NS
		? state->draw_time / 2 : JIT_MIN_MARGIN_NS;
	return render_at - state->draw_time - margin;
}

int64_t feedback_frame_done(const struct flame_element_list *list, struct wlr_surface *plane,
		uint64_t cycle, int64_t now, int64_t render_at) {
	int64_t next = 0;
	struct flame_surface_state *plane_state = plane ? scene_surface_state(plane) : NULL;
	if (plane_state && plane_state->callback_cycle < cycle) {
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
		} else if (state->callback_cycle < cycle) {
			// cycles only grow: one served early for the next is skipped too
			// its turn this cycle, even with nothing asked for yet: asking
			// later waits for the next cycle, nobody wakes twice a refresh.
			// one that draws fast waits until just in time
			int64_t wake = wake_time(state, render_at);
			// once picked, the wake only ever comes sooner: a recount after
			// a flip aims at the render after, and with something flipping
			// every refresh (the mouse) it would never come at all
			if (wake && state->jit_wake && state->jit_wake < wake) {
				wake = state->jit_wake;
			}
			state->jit_wake = wake;
			if (wake > now) {
				next = !next || wake < next ? wake : next;
			} else {
				send(state, cycle, now);
				state->jit_target = wake ? render_at : 0;
			}
		}
	}
	return next;
}

bool feedback_waiting_next_cycle(const struct flame_element_list *list, uint64_t cycle) {
	for (size_t i = 0; i < list->len; i++) {
		const struct flame_element *element = &list->data[i];
		if (element->type != FLAME_ELEMENT_SURFACE
				|| !pixman_region32_not_empty(&element->visible)
				|| wl_list_empty(&element->surface->current.frame_callback_list)) {
			continue;
		}
		struct flame_surface_state *state = scene_surface_state(element->surface);
		if (state && state->callback_cycle >= cycle) {
			return true;
		}
	}
	return false;
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
