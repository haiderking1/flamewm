#ifndef FLAME_RENDER_FRAME_H
#define FLAME_RENDER_FRAME_H

#include <pixman.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_damage_ring.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/box.h>

#include "render/elements.h"
#include "render/gpu_time.h"
#include "render/scanout.h"
#include "render/schedule.h"
#include "render/timing.h"
#include "util/timer.h"

struct flame_render;
struct wlr_session;

// what the output around a frame wants to hear about
struct flame_frame_hooks {
	void *data;
	// right before a frame gets built from the scene (a mouse resize lands here)
	void (*before_render)(void *data);
	// frame callbacks just went out (transactions keep hidden windows drawing)
	void (*callbacks_sent)(void *data, const struct timespec *now);
};

// one output's frames: when to draw (schedule.h decides), what it showed last
// time and what changed since, the frame waiting behind a pending flip, and
// the timelines explicit sync hangs off. flame drives this, wlroots' frame
// event only tells it a flip finished
struct flame_frame {
	struct wlr_output *output;
	struct flame_render *render;
	const struct wlr_box *area; // the layout part the output shows
	struct flame_frame_hooks hooks;

	struct schedule schedule;
	struct flame_timer wake; // render window, callback wake, watchdog
	struct wl_event_source *idle; // requests are handled once per dispatch
	bool stall_reported;
	// the latest present's kernel timestamp, for the flip that follows it
	int64_t present_ns;

	// tickets for the schedule, only their addresses matter
	int submitted_ticket, queued_ticket;
	// a successor, rendered while the pending one is still waiting to flip
	struct wlr_output_state queued;
	bool has_queued;
	bool queued_scanout; // it's a client buffer going straight to KMS

	struct flame_scanout scanout;

	struct wlr_damage_ring ring; // buffer coords, per swapchain buffer
	pixman_region32_t pending; // layout coords, from client commits
	pixman_region32_t pending_buffer; // buffer coords, from wlroots (cursor)
	bool whole; // mode, scale or transform changed, or the first frame

	struct flame_element_list prev, cur;

	// explicit sync: our drawing finished / KMS let go of a buffer
	struct wlr_drm_syncobj_timeline *in_timeline, *out_timeline;
	uint64_t in_point, out_point;

	struct flame_frame_timing timing; // FLAME_FRAME_TIMING probes
	struct flame_gpu_time gpu; // for the schedule's lead and the probes

	struct wl_listener scene_changed;
	struct wl_listener scene_damage;
	struct wl_listener output_damage;
	struct wl_listener output_needs_frame;
	struct wl_listener output_commit;
	struct wl_listener output_frame;
	struct wl_listener output_present;
	struct wl_listener session_active;
};

// session may be NULL (nested), area is read every frame
bool frame_init(struct flame_frame *frame, struct flame_render *render,
	struct wlr_output *output, struct wlr_session *session, const struct wlr_box *area,
	struct flame_frame_hooks hooks);
void frame_finish(struct flame_frame *frame);

// something wants a frame: the schedule decides when
void frame_request(struct flame_frame *frame);

#endif
