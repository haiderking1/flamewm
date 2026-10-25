#include "scene/surface.h"

#include <stdio.h>
#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_xdg_shell.h>
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

// which app a surface is part of: its window's app id, else its title
static void app_name(struct wlr_surface *surface, char *out, size_t size) {
	struct wlr_xdg_toplevel *toplevel =
		wlr_xdg_toplevel_try_from_wlr_surface(wlr_surface_get_root_surface(surface));
	snprintf(out, size, "%s", !toplevel ? "?" : toplevel->app_id ? toplevel->app_id
		: toplevel->title ? toplevel->title : "?");
}

// a buffer from surface while the scene counts
static void count_buffer(struct flame_scene *scene, struct wlr_surface *surface) {
	struct wlr_surface *root = wlr_surface_get_root_surface(surface);
	for (int i = 0; i < SCENE_COUNTED; i++) {
		if (scene->counted[i].root == root) {
			scene->counted[i].buffers++;
			return;
		}
		if (!scene->counted[i].root) {
			app_name(root, scene->counted[i].app_id, sizeof(scene->counted[i].app_id));
			scene->counted[i].root = root;
			scene->counted[i].buffers = 1;
			return;
		}
	}
}

// what drawing steadily is (under this between buffers), how long a gap is
// worth a line, and past what it's a pause, not a stall (a video paused)
#define STEADY_NS 100000000LL
#define STALL_NS 500000000LL
#define STALL_MAX_NS 30000000000LL

void scene_surface_frame_done(struct wlr_surface *surface, const struct timespec *when) {
	struct flame_surface_state *state = scene_surface_state(surface);
	if (state) {
		state->answered_at = now_ns();
	}
	wlr_surface_send_frame_done(surface, when);
}

// a new buffer, now: after a gap in an app that was drawing steadily and
// showed, whose wait it was. flame owed it the frame callback it asked for,
// or it was busy drawing, or it never asked
static void check_stall(struct flame_surface_state *state, int64_t now) {
	int64_t gap = state->last_buffer ? now - state->last_buffer : 0;
	bool seen = scene_surface_drawn(state) && !wlr_box_empty(&state->drawn_visible);
	if (seen && state->buffer_gap && state->buffer_gap < STEADY_NS && gap > STALL_NS
			&& gap < STALL_MAX_NS) {
		char name[64];
		app_name(state->surface, name, sizeof(name));
		// asked with its last buffer or after it: that callback was the wait
		bool asked = state->asked_at && state->asked_at >= state->last_buffer;
		if (!asked) {
			wlr_log(WLR_INFO, "stall: %s went %.1fs without a new frame, it never asked flame "
				"for one", name, gap / 1e9);
		} else if (state->answered_at < state->asked_at) {
			wlr_log(WLR_ERROR, "stall: %s went %.1fs without a new frame, flame never "
				"answered the frame callback it asked for", name, gap / 1e9);
		} else {
			double flame = (state->answered_at - state->asked_at) / 1e9;
			double app = (now - state->answered_at) / 1e9;
			wlr_log(flame > app ? WLR_ERROR : WLR_INFO, "stall: %s went %.1fs without a new "
				"frame: flame took %.2fs to say draw, %s took %.2fs to draw after", name,
				gap / 1e9, flame, name, app);
		}
	}
	if (gap > 0 && gap < 1000000000LL) {
		state->buffer_gap = state->buffer_gap ? state->buffer_gap + (gap - state->buffer_gap) / 8
			: gap;
	}
	state->last_buffer = now;
}

static void handle_commit(struct wl_listener *listener, void *data) {
	struct flame_surface_state *state = wl_container_of(listener, state, commit);
	wl_signal_emit_mutable(&state->scene->events.commit, state->surface);
	if (state->surface->current.committed & WLR_SURFACE_STATE_BUFFER) {
		if (state->scene->counting) {
			count_buffer(state->scene, state->surface);
		}
		int64_t now = now_ns();
		check_stall(state, now);
		state->buffer_since = now;
		if (state->drawing && state->callback_time) {
			// an app idle for a while (over 50ms: a terminal waiting on a
			// key) isn't drawing frame after frame, that's no sample. taken
			// whole it would make it slow for good
			int64_t took = now - state->callback_time;
			if (took < 50000000LL) {
				state->draw_time = took > state->draw_time
					? took : state->draw_time + (took - state->draw_time) / 8;
			}
			state->drawing = false;
		}
		if (state->jit_target && now > state->jit_target) {
			state->scene->late_app_frames++;
		}
		state->jit_target = 0;
	}
	if (!wl_list_empty(&state->surface->current.frame_callback_list)
			&& (!state->asked_at || state->answered_at >= state->asked_at)) {
		state->asked_at = now_ns();
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
