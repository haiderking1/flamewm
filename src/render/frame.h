#ifndef FLAME_RENDER_FRAME_H
#define FLAME_RENDER_FRAME_H

#include <pixman.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_damage_ring.h>
#include <wlr/util/box.h>

#include "render/elements.h"

struct flame_render;
struct wlr_output;

// one output's frames: what it showed last time, what changed since, and the
// timelines explicit sync hangs off. builds a frame from the scene, draws only
// what changed and commits it
struct flame_frame {
	struct wlr_output *output;
	struct flame_render *render;

	struct wlr_damage_ring ring; // buffer coords, per swapchain buffer
	pixman_region32_t pending; // layout coords, from client commits
	pixman_region32_t pending_buffer; // buffer coords, from wlroots (cursor)
	bool whole; // mode, scale or transform changed, or the first frame

	struct flame_element_list prev, cur;

	// explicit sync: our drawing finished / KMS let go of a buffer
	struct wlr_drm_syncobj_timeline *in_timeline, *out_timeline;
	uint64_t in_point, out_point;

	struct wl_listener scene_changed;
	struct wl_listener scene_damage;
	struct wl_listener output_damage;
	struct wl_listener output_needs_frame;
	struct wl_listener output_commit;
};

bool frame_init(struct flame_frame *frame, struct flame_render *render,
	struct wlr_output *output);
void frame_finish(struct flame_frame *frame);

// build, draw and commit a frame for the part of the layout the output shows.
// nothing changed means nothing gets drawn, callbacks still go out
void frame_render(struct flame_frame *frame, struct wlr_box logical,
	const struct timespec *now);

#endif
