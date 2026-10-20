#ifndef FLAME_RENDER_TIMING_H
#define FLAME_RENDER_TIMING_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <wayland-server-core.h>

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
	struct wl_event_source *report;
	struct timespec span_start;

	// this report
	uint32_t flips, composited, failed;
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

void timing_init(struct flame_frame_timing *timing, struct wlr_output *output, bool enabled);
void timing_finish(struct flame_frame_timing *timing);

// one call to frame_render, from start to end. drew: a changed frame got
// queued, else it was an empty pass (no damage, nothing queued)
void timing_frame(struct flame_frame_timing *timing, const struct timespec *start,
	const struct timespec *end, bool drew);

// a frame went to KMS, or didn't
void timing_committed(struct flame_frame_timing *timing, bool ok);
// gpu_time_collect's longest finished measurement
void timing_gpu(struct flame_frame_timing *timing, double max_ms);

#endif
