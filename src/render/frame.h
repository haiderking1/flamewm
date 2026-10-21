#ifndef FLAME_RENDER_FRAME_H
#define FLAME_RENDER_FRAME_H

#include <pixman.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/types/wlr_damage_ring.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/box.h>

#include "render/cursor.h"
#include "render/elements.h"
#include "render/gpu_time.h"
#include "render/hiccup.h"
#include "render/scanout.h"
#include "render/schedule.h"
#include "render/sync.h"
#include "render/timing.h"
#include "util/timer.h"

struct flame_render;
struct wlr_session;

// a frame reached the screen, see events.presented
struct flame_frame_presented {
	uint64_t draw_id; // what buffer nodes were told when they were drawn
	bool presented; // false: it was dropped, never shown
	int64_t when; // kernel flip timestamp, CLOCK_MONOTONIC ns
	uint32_t seq; // vblank counter
	int refresh; // ns, 0 for unknown
};

// what the output around a frame wants to hear about
struct flame_frame_hooks {
	void *data;
	// right before a frame gets built from the scene (a mouse resize lands
	// here, animations step to target: when this frame should be on screen).
	// true: the next frame is wanted too. asking from in here any other way
	// is lost, this frame's submission clears it
	bool (*before_render)(void *data, int64_t target);
	// frame callbacks just went out (transactions keep hidden windows drawing).
	// new_cycle: a submission or the refresh grid moving on, at most once a
	// refresh. anything re-requesting frames from here has to wait for one, a
	// revisit comes right after every empty pass and would loop
	void (*callbacks_sent)(void *data, const struct timespec *now, bool new_cycle);
	// a fullscreen window shows on this output right now (vrr's fullscreen
	// policy). NULL: never
	bool (*fullscreen)(void *data);
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
	// inside process(): requests made from in there wait for its timer, so
	// nothing it triggers can bounce straight back into it and spin the loop
	bool processing;
	bool stall_reported;
	// the latest present's kernel timestamp, for the flip that follows it
	int64_t present_ns;
	// frame callback cycles, see schedule_cycle
	uint64_t callback_cycle;
	// surfaces of this cycle waiting to be woken just in time, 0 for none
	int64_t jit_wake;

	// tickets for the schedule, only their addresses matter. foreign: a
	// commit that wasn't ours (the modeset turning the output on) has a flip
	// in flight too, the first frame waits for it
	int submitted_ticket, queued_ticket, foreign_ticket;
	bool committing; // inside our own wlr_output_commit_state
	// a successor, rendered while the pending one is still waiting to flip
	struct wlr_output_state queued;
	bool has_queued;
	enum timing_commit queued_kind;
	// the buffer of the newest commit: a frame where only the cursor plane
	// changed sends it again instead of drawing anything
	struct wlr_buffer *front;

	struct flame_scanout scanout;
	struct flame_frame_cursor cursor;
	// tearing and vrr, see render/sync.h. tearing: the fullscreen app wants
	// it and gets it (or would, KMS willing): frames go out asap
	struct sync sync;
	bool tearing_logged; // KMS refusing it was said once
	bool vrr_logged;

	struct wlr_damage_ring ring; // buffer coords, per swapchain buffer
	pixman_region32_t pending; // layout coords, from client commits
	bool whole; // mode, scale or transform changed, or the first frame

	struct flame_element_list prev, cur;

	// output sized buffers for close animations, see render/snapshot.c
	struct wlr_swapchain *snapshots;

	// explicit sync: our drawing finished / KMS let go of a buffer
	struct wlr_drm_syncobj_timeline *in_timeline, *out_timeline;
	uint64_t in_point, out_point;

	// every drawn frame gets a number, buffer nodes hear it when they're drawn
	// and events.presented says when that number reached the screen
	uint64_t draw_id;
	uint64_t pending_draw, queued_draw; // the KMS slot's and the successor's
	// wayland buffers drawn for the first time: latency once their flip lands
	struct {
		uint64_t draw_id;
		int64_t since;
	} first_drawn[32];
	int first_drawn_len;
	struct {
		struct wl_signal presented; // struct flame_frame_presented
		struct wl_signal destroy;
	} events;

	struct flame_frame_timing timing; // FLAME_FRAME_TIMING probes
	struct flame_gpu_time gpu; // for the probes, and the lead without done_waiter
	struct flame_hiccup hiccup; // the driver's draw stalls, see render/hiccup.h
	// a composited frame's gpu work, handed to KMS -> done. that's what KMS
	// waits for, queueing behind other apps' gpu work included, so it's the
	// gpu part of the schedule's lead. one frame watched at a time
	struct wlr_drm_syncobj_timeline_waiter done_waiter;
	bool done_waiting;
	int64_t done_since;

	struct wl_listener scene_changed;
	struct wl_listener scene_damage;
	struct wl_listener surface_destroy;
	struct wl_listener pointer_changed;
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
