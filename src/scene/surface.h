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
	// where that was, layout coords: the surface itself and the part that showed
	int drawn_x, drawn_y;
	struct wlr_box drawn_clip;
	// the output it was told it's on, NULL for none
	struct wlr_output *entered;
	// the scene node drawing it, when it's the root of one
	struct flame_surface_node *node;

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
