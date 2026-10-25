#ifndef FLAME_RENDER_TIMING_H
#define FLAME_RENDER_TIMING_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <wayland-server-core.h>

struct schedule;
struct wlr_output;

// frame timing probes: start with
// FLAME_FRAME_TIMING=1 and every two seconds each output logs a
// "flame: frame-timing" line. observations only, nothing about rendering or
// pacing changes, constant memory, no logging per frame

// min/mean/max in milliseconds, nothing for no samples
struct timing_stat {
	uint32_t count;
	double min, max, sum;
};

struct flame_frame_timing {
	bool enabled;
	struct wlr_output *output;
	const struct schedule *schedule; // its policy, overlap and lead get reported
	struct wl_event_source *report;
	struct timespec span_start;

	// this report
	uint32_t flips, composited, scanout, cursor, failed, tearing;
	double pace_fps; // the game paced on this output, 0 for none (frame.pace_fps)
	uint32_t pace_stuck; // times it waited on buffers the pacer held, all given back
	// successors: rendered behind a pending flip, sent when it landed, or
	// refused by KMS right then
	uint32_t prepared, promoted, discarded;
	// the schedule's timer firing after its deadline, and before it
	struct timing_stat timer_late;
	uint32_t timer_early;
	// gpu: the longest finished measurement per collection, not per
	// frame, results arrive a frame or two late
	struct timing_stat draw, empty, gpu, flip, dispatch;
	// thousands of pixels a composited frame: what changed, what got redrawn
	// (that grown by what the buffer missed, and the glass over it)
	struct timing_stat damage_kpx, repaint_kpx;
	// a client's new buffer handed over -> the flip that first showed it
	struct timing_stat wayland_latency, x11_latency;
	// the focused fullscreen X game's own present to present: a hitch in
	// the game, flips include an idle desktop's
	struct timing_stat game_gap;
	// a composited frame handed to KMS -> our gpu work for it done: what KMS
	// waits for, the wait behind other apps' gpu work included
	struct timing_stat gpu_done;
	// vblanks our frames missed, how early they were submitted for theirs,
	// app frames that came in after the render they were woken for
	uint32_t missed, late_app_frames;
	// the driver's periodic draw stalls, and how many were planned for
	uint32_t hiccups, hiccups_planned;
	struct timing_stat slack;
	uint32_t seq_steps[3]; // vblanks between flips: 1, 2, 3 or more
	uint32_t seq_repeat, seq_reset, metadata_missing, clock_discontinuities;
	bool hw_clock; // the newest flip carried a hardware timestamp

	// carried across reports, so a flip interval over a boundary still counts
	bool have_flip;
	struct timespec last_flip;
	unsigned last_seq;

	struct wl_listener present;
};

// FLAME_FRAME_TIMING: 1/true on, unset/0/false off
bool timing_enabled_by_env(void);

void timing_init(struct flame_frame_timing *timing, struct wlr_output *output,
	const struct schedule *schedule, bool enabled);
void timing_finish(struct flame_frame_timing *timing);
// the probes on or off while flame runs (flamectl frame-timing), a report
// every 2s in the log while they're on
void timing_set(struct flame_frame_timing *timing, bool on);
// a composited frame's changed and redrawn areas, in pixels
void timing_areas(struct flame_frame_timing *timing, uint64_t damage, uint64_t repaint);

// one call to frame_render, from start to end. drew: a changed frame got
// queued, else it was an empty pass (no damage, nothing queued)
void timing_frame(struct flame_frame_timing *timing, const struct timespec *start,
	const struct timespec *end, bool drew);

// what a commit carried
enum timing_commit {
	TIMING_COMPOSITED, // a frame we drew
	TIMING_SCANOUT, // a client buffer straight to KMS
	TIMING_CURSOR, // only the cursor plane moved, the same buffer again
};

// a frame went to KMS, or didn't
void timing_committed(struct flame_frame_timing *timing, bool ok, enum timing_commit kind);
// that commit went out as a tearing flip
void timing_tearing(struct flame_frame_timing *timing);
void timing_successor_prepared(struct flame_frame_timing *timing);
// a successor went to KMS when the flip landed (ok) or got refused
void timing_successor(struct flame_frame_timing *timing, bool ok);
// the schedule's timer fired, late_ns after (negative: before) its deadline
void timing_wake(struct flame_frame_timing *timing, int64_t late_ns);
// a frame went to KMS slack ns before the vblank it's meant for
void timing_slack(struct flame_frame_timing *timing, int64_t ns);
// a flip landed a refresh late, the app frames that missed their render
void timing_missed(struct flame_frame_timing *timing);
void timing_late_app_frames(struct flame_frame_timing *timing, uint32_t count);
// a client buffer reached the screen ns after it was handed over: a wayland
// commit, or an X11 PresentPixmap
void timing_latency(struct flame_frame_timing *timing, bool x11, int64_t ns);
// the fullscreen game presented ns after its last present
void timing_game_gap(struct flame_frame_timing *timing, int64_t ns);
// gpu_time_collect's longest finished measurement
void timing_gpu(struct flame_frame_timing *timing, double max_ms);
void timing_gpu_done(struct flame_frame_timing *timing, int64_t ns);
void timing_hiccup(struct flame_frame_timing *timing, bool planned);

#endif
