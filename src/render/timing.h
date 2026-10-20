#ifndef FLAME_RENDER_TIMING_H
#define FLAME_RENDER_TIMING_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <wayland-server-core.h>

struct schedule;
struct wlr_output;

// frame timing probes, raven's RAVEN_FRAME_TIMING for flame: start with
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
	uint32_t flips, composited, scanout, failed;
	// successors: rendered behind a pending flip, sent when it landed, or
	// refused by KMS right then
	uint32_t prepared, promoted, discarded;
	// the schedule's timer firing after its deadline, and before it
	struct timing_stat timer_late;
	uint32_t timer_early;
	// gpu: the longest finished measurement per collection, like raven's
	// gpu_batch_max_ms. not per frame, results arrive a frame or two late
	struct timing_stat draw, empty, gpu, flip, dispatch;
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

// one call to frame_render, from start to end. drew: a changed frame got
// queued, else it was an empty pass (no damage, nothing queued)
void timing_frame(struct flame_frame_timing *timing, const struct timespec *start,
	const struct timespec *end, bool drew);

// a frame went to KMS (composited by us or a client buffer scanned out), or didn't
void timing_committed(struct flame_frame_timing *timing, bool ok, bool scanout);
void timing_successor_prepared(struct flame_frame_timing *timing);
// a successor went to KMS when the flip landed (ok) or got refused
void timing_successor(struct flame_frame_timing *timing, bool ok);
// the schedule's timer fired, late_ns after (negative: before) its deadline
void timing_wake(struct flame_frame_timing *timing, int64_t late_ns);
// gpu_time_collect's longest finished measurement
void timing_gpu(struct flame_frame_timing *timing, double max_ms);

#endif
