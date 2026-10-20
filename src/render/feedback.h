#ifndef FLAME_RENDER_FEEDBACK_H
#define FLAME_RENDER_FEEDBACK_H

#include <stdint.h>
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

// frame callbacks for everything that actually shows, one turn per surface
// per cycle (frame.c bumps cycle for a new one). covered surfaces wait
void feedback_frame_done(const struct flame_element_list *list, uint64_t cycle, int64_t now);

// covered windows asking for callbacks get one every 250ms, so they don't
// freeze without running at the refresh rate behind something opaque.
// never asks for a repaint. returns when the next one is due, 0 for none.
// hidden workspaces aren't in the list and get nothing
int64_t feedback_background(const struct flame_element_list *list, uint64_t cycle, int64_t now);

#endif
