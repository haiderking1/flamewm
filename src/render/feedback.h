#ifndef FLAME_RENDER_FEEDBACK_H
#define FLAME_RENDER_FEEDBACK_H

#include <time.h>

struct flame_element_list;
struct flame_scene;
struct flame_scene_surfaces;
struct wlr_output;

// what clients hear about a frame of output. scene tracked this for us before

// the frame's surfaces are on output now: remembered for damage and
// wl_surface.enter, with the output's scale. the ones that aren't anymore get
// wl_surface.leave. bumps the scene's frame_seq
void feedback_drawn(struct flame_scene_surfaces *surfaces, struct wlr_output *output,
	const struct flame_element_list *list);

// frame callbacks for everything that actually shows, covered ones wait
void feedback_frame_done(const struct flame_element_list *list, const struct timespec *now);

#endif
