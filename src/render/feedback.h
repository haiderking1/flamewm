#ifndef FLAME_RENDER_FEEDBACK_H
#define FLAME_RENDER_FEEDBACK_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

struct flame_element_list;
struct flame_scene;
struct flame_scene_surfaces;
struct wlr_output;
struct wlr_surface;

// what clients hear about a frame of output. scene tracked this for us before

// the frame's surfaces are on output now: remembered for damage and
// wl_surface.enter, with the output's scale. the ones that aren't anymore get
// wl_surface.leave. bumps the scene's frame_seq. plane is a client cursor on
// the cursor plane (or NULL): on screen, just not something we draw
void feedback_drawn(struct flame_scene_surfaces *surfaces, struct wlr_output *output,
	const struct flame_element_list *list, struct wlr_surface *plane);

// frame callbacks for everything that actually shows, one turn per surface
// per cycle (frame.c bumps cycle for a new one). covered surfaces wait.
// render_at (0 for none) is when flame renders the frame they can still make:
// a surface whose draw time is known is woken just in time for it, drawing
// as late as it can with the freshest input. returns when the next of those
// is due, 0 for none, call again then with the same cycle
int64_t feedback_frame_done(const struct flame_element_list *list, struct wlr_surface *plane,
	uint64_t cycle, int64_t now, int64_t render_at);

// a visible surface asks for a callback but had this cycle's turn already
// (it asked again right after, or was served early for it): only the next
// cycle serves it, so the next refresh has to start one even if nothing flips
bool feedback_waiting_next_cycle(const struct flame_element_list *list, uint64_t cycle);

// right after a submission: surfaces too slow to draw a frame in budget ns
// (what's left of a refresh after flame's own render) get next_cycle's turn
// now instead of at the flip, so they keep their frame rate. everyone else
// waits for the flip, a frame of latency less
void feedback_frame_done_early(const struct flame_element_list *list, uint64_t next_cycle,
	int64_t now, int64_t budget);

// covered windows asking for callbacks get one every 250ms, so they don't
// freeze without running at the refresh rate behind something opaque.
// never asks for a repaint. returns when the next one is due, 0 for none.
// hidden workspaces aren't in the list and get nothing
int64_t feedback_background(const struct flame_element_list *list, uint64_t cycle, int64_t now);

#endif
