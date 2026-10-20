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
		const struct flame_element_list *list) {
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
		state->drawn_clip = element->clip;
		enter(state, output);
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

void feedback_frame_done(const struct flame_element_list *list, const struct timespec *now) {
	for (size_t i = 0; i < list->len; i++) {
		const struct flame_element *element = &list->data[i];
		if (element->type == FLAME_ELEMENT_SURFACE
				&& pixman_region32_not_empty(&element->visible)) {
			wlr_surface_send_frame_done(element->surface, now);
		}
	}
}
