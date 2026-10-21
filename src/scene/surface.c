#include "scene/surface.h"

#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/util/log.h>

#include "scene/scene.h"
#include "util/listener.h"
#include "util/timer.h"

static void addon_destroy(struct wlr_addon *addon);

static const struct wlr_addon_interface addon_impl = {
	.name = "flame_surface_state",
	.destroy = addon_destroy,
};

struct flame_surface_state *scene_surface_state(struct wlr_surface *surface) {
	// the interface doubles as the owner key, there's one of us per surface
	struct wlr_addon *addon = wlr_addon_find(&surface->addons, &addon_impl, &addon_impl);
	if (!addon) {
		return NULL;
	}
	struct flame_surface_state *state = wl_container_of(addon, state, addon);
	return state;
}

static void addon_destroy(struct wlr_addon *addon) {
	struct flame_surface_state *state = wl_container_of(addon, state, addon);
	wl_signal_emit_mutable(&state->scene->events.surface_destroy, state->surface);
	listener_disconnect(&state->commit);
	wl_list_remove(&state->link);
	wlr_addon_finish(&state->addon);
	if (scene_surface_drawn(state)) {
		// wherever it was has to be redrawn without it
		scene_changed(state->scene);
	}
	free(state);
}

bool scene_surface_drawn(const struct flame_surface_state *state) {
	return state->drawn_seq != 0 && state->drawn_seq == state->scene->frame_seq;
}

// on screen already, or the root of something that's about to be
static bool shown(struct flame_surface_state *state) {
	if (scene_surface_drawn(state)) {
		return true;
	}
	struct flame_surface_state *root =
		scene_surface_state(wlr_surface_get_root_surface(state->surface));
	if (!root) {
		return false;
	}
	int lx, ly;
	return scene_surface_drawn(root)
		|| (root->node && scene_node_coords(&root->node->node, &lx, &ly));
}

static void handle_commit(struct wl_listener *listener, void *data) {
	struct flame_surface_state *state = wl_container_of(listener, state, commit);
	wl_signal_emit_mutable(&state->scene->events.commit, state->surface);
	if (state->surface->current.committed & WLR_SURFACE_STATE_BUFFER) {
		int64_t now = now_ns();
		state->buffer_since = now;
		if (state->drawing && state->callback_time) {
			int64_t took = now - state->callback_time;
			state->draw_time = took > state->draw_time
				? took : state->draw_time + (took - state->draw_time) / 8;
			state->drawing = false;
		}
		if (state->jit_target && now > state->jit_target) {
			state->scene->late_app_frames++;
		}
		state->jit_target = 0;
	}
	// what it redrew, where it was last drawn, only where it showed: a window
	// covered by something opaque can redraw all it wants without a repaint.
	// moving or resizing it is the frame's business, it compares frames
	if (scene_surface_drawn(state) && !wlr_box_empty(&state->drawn_visible)) {
		pixman_region32_t damage;
		pixman_region32_init(&damage);
		wlr_surface_get_effective_damage(state->surface, &damage);
		pixman_region32_translate(&damage, state->drawn_x, state->drawn_y);
		pixman_region32_intersect_rect(&damage, &damage, state->drawn_visible.x,
			state->drawn_visible.y, state->drawn_visible.width, state->drawn_visible.height);
		if (pixman_region32_not_empty(&damage)) {
			wl_signal_emit_mutable(&state->scene->events.damage, &damage);
		}
		pixman_region32_fini(&damage);
	}
	// hidden workspaces and the staging tree don't wake the outputs
	if (shown(state)) {
		scene_changed(state->scene);
	}
}

static void handle_new_surface(struct wl_listener *listener, void *data) {
	struct flame_scene_surfaces *surfaces = wl_container_of(listener, surfaces, new_surface);
	struct wlr_surface *surface = data;

	struct flame_surface_state *state = calloc(1, sizeof(*state));
	if (!state) {
		// it still works, it just won't wake the outputs on its own
		wlr_log(WLR_ERROR, "out of memory");
		return;
	}
	state->scene = surfaces->scene;
	state->surface = surface;
	wlr_addon_init(&state->addon, &surface->addons, &addon_impl, &addon_impl);
	listener_connect(&state->commit, &surface->events.commit, handle_commit);
	wl_list_insert(&surfaces->states, &state->link);
}

void scene_surfaces_init(struct flame_scene_surfaces *surfaces, struct flame_scene *scene,
		struct wlr_compositor *compositor) {
	surfaces->scene = scene;
	wl_list_init(&surfaces->states);
	listener_connect(&surfaces->new_surface, &compositor->events.new_surface,
		handle_new_surface);
}

void scene_surfaces_finish(struct flame_scene_surfaces *surfaces) {
	listener_disconnect(&surfaces->new_surface);
}

void scene_surfaces_output_gone(struct flame_scene_surfaces *surfaces,
		struct wlr_output *output) {
	struct flame_surface_state *state;
	wl_list_for_each(state, &surfaces->states, link) {
		if (state->entered == output) {
			state->entered = NULL;
		}
	}
}
