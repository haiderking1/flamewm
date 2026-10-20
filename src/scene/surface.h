#ifndef FLAME_SCENE_SURFACE_H
#define FLAME_SCENE_SURFACE_H

#include <pixman.h>
#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/util/addon.h>
#include <wlr/util/box.h>

struct flame_scene;
struct flame_surface_node;
struct wlr_compositor;
struct wlr_output;
struct wlr_surface;

// what flame knows about every client surface: whether it was in the last
// frame and what it redrew since. render/ fills in the drawing side, commits
// fill in the rest. every wl_surface gets one the moment it's created, so even
// a subsurface or popup we haven't drawn yet asks for a frame when it maps
struct flame_surface_state {
	struct wl_list link; // flame_scene_surfaces.states
	struct flame_scene *scene;
	struct wlr_surface *surface;
	struct wlr_addon addon;

	// the last frame it was in (on screen, maybe covered), see flame_scene.frame_seq
	uint64_t drawn_seq;
	// where that was, layout coords: the surface itself, and the bounding box
	// of what actually showed (empty when something opaque covered it all)
	int drawn_x, drawn_y;
	struct wlr_box drawn_visible;

	// frame callbacks: the cycle it last had its turn in, and when (ns),
	// or since when it's been waiting while covered
	uint64_t callback_cycle;
	int64_t callback_time;
	// the output it was told it's on, NULL for none
	struct wlr_output *entered;
	// the scene node drawing it, when it's the root of one
	struct flame_surface_node *node;
	// when its newest buffer came in, ns, 0 once a frame drew it. for the
	// latency probe
	int64_t buffer_since;
	// how long it takes from a frame callback to its next buffer, ns: quick
	// to grow, slow to shrink. apps that can't make the next vblank when woken
	// on the flip get their turn earlier, at the submission
	int64_t draw_time;
	bool drawing; // woken, its buffer hasn't come in yet
	// woken just in time for the render at this time, 0 when it wasn't
	int64_t jit_target;
	// the just in time wake it's waiting for, 0 for none. a deadline: frames
	// going out meanwhile move the render it aims at, never this later
	int64_t jit_wake;

	struct wl_listener commit;
};

struct flame_scene_surfaces {
	struct flame_scene *scene;
	struct wl_list states; // flame_surface_state.link
	struct wl_listener new_surface;
};

void scene_surfaces_init(struct flame_scene_surfaces *surfaces, struct flame_scene *scene,
	struct wlr_compositor *compositor);
void scene_surfaces_finish(struct flame_scene_surfaces *surfaces);

// NULL for surfaces of another compositor (there are none)
struct flame_surface_state *scene_surface_state(struct wlr_surface *surface);
// it was in the newest frame
bool scene_surface_drawn(const struct flame_surface_state *state);

// an output is going away, nobody is on it anymore
void scene_surfaces_output_gone(struct flame_scene_surfaces *surfaces,
	struct wlr_output *output);

#endif
