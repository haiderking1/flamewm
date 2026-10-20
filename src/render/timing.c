#include "render/timing.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

#include "render/schedule.h"
#include "util/listener.h"

#define REPORT_MS 2000

bool timing_enabled_by_env(void) {
	const char *value = getenv("FLAME_FRAME_TIMING");
	if (!value || !strcmp(value, "0") || !strcasecmp(value, "false")) {
		return false;
	}
	if (!strcmp(value, "1") || !strcasecmp(value, "true")) {
		return true;
	}
	wlr_log(WLR_ERROR, "FLAME_FRAME_TIMING=%s isn't 1/true/0/false, timing stays off", value);
	return false;
}

static double ms_between(const struct timespec *a, const struct timespec *b) {
	return (double)(b->tv_sec - a->tv_sec) * 1e3 + (double)(b->tv_nsec - a->tv_nsec) / 1e6;
}

static void stat_add(struct timing_stat *stat, double ms) {
	if (stat->count == 0 || ms < stat->min) {
		stat->min = ms;
	}
	if (stat->count == 0 || ms > stat->max) {
		stat->max = ms;
	}
	stat->sum += ms;
	stat->count++;
}

static const char *stat_str(const struct timing_stat *stat, char *buf, size_t size) {
	if (stat->count == 0) {
		return "n/a";
	}
	snprintf(buf, size, "%.3f/%.3f/%.3f", stat->min, stat->sum / stat->count, stat->max);
	return buf;
}

// ---- pageflips ----

static void handle_present(struct wl_listener *listener, void *data) {
	struct flame_frame_timing *timing = wl_container_of(listener, timing, present);
	const struct wlr_output_event_present *event = data;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	if (!event->presented) {
		return;
	}
	timing->flips++;
	if (event->when.tv_sec == 0 && event->when.tv_nsec == 0) {
		// no timestamp, the interval history breaks here
		timing->metadata_missing++;
		timing->have_flip = false;
		return;
	}
	timing->hw_clock = event->flags & WLR_OUTPUT_PRESENT_HW_CLOCK;

	if (timing->have_flip) {
		double interval = ms_between(&timing->last_flip, &event->when);
		if (interval > 0) {
			stat_add(&timing->flip, interval);
		} else {
			timing->clock_discontinuities++;
		}
		if (event->seq && timing->last_seq) {
			if (event->seq == timing->last_seq) {
				timing->seq_repeat++;
			} else if (event->seq < timing->last_seq) {
				timing->seq_reset++;
			} else {
				unsigned step = event->seq - timing->last_seq;
				timing->seq_steps[step >= 3 ? 2 : step - 1]++;
			}
		}
	}
	double dispatch = ms_between(&event->when, &now);
	if (dispatch >= 0) {
		stat_add(&timing->dispatch, dispatch);
	}
	timing->last_flip = event->when;
	timing->last_seq = event->seq;
	timing->have_flip = true;
}

// ---- reports ----

static void reset_span(struct flame_frame_timing *timing) {
	timing->flips = timing->composited = timing->failed = 0;
	timing->prepared = timing->promoted = timing->discarded = 0;
	memset(&timing->timer_late, 0, sizeof(timing->timer_late));
	timing->timer_early = 0;
	memset(&timing->draw, 0, sizeof(timing->draw));
	memset(&timing->empty, 0, sizeof(timing->empty));
	memset(&timing->gpu, 0, sizeof(timing->gpu));
	memset(&timing->flip, 0, sizeof(timing->flip));
	memset(&timing->dispatch, 0, sizeof(timing->dispatch));
	memset(timing->seq_steps, 0, sizeof(timing->seq_steps));
	timing->seq_repeat = timing->seq_reset = 0;
	timing->metadata_missing = timing->clock_discontinuities = 0;
	clock_gettime(CLOCK_MONOTONIC, &timing->span_start);
}

static int handle_report(void *data) {
	struct flame_frame_timing *timing = data;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	double span = ms_between(&timing->span_start, &now) / 1e3;

	char draw[64], empty[64], gpu[64], flip[64], dispatch[64], late[64];
	const struct schedule *schedule = timing->schedule;
	wlr_log(WLR_INFO, "flame: frame-timing output=%s mode=%.3fHz span=%.2fs "
		"pipeline=%s overlap=%s lead_ms=%.3f "
		"flips=%u flip_hz=%.1f composited=%u failed=%u "
		"successors=%u promoted=%u discarded=%u timer_late_ms=%s timer_early=%u "
		"draws=%u draw_ms=%s empty=%u empty_ms=%s gpu_batch_max_ms=%s flip_ms=%s "
		"seq_steps=1:%u,2:%u,3+:%u dispatch_ms=%s clock=%s "
		"metadata_missing=%u clock_discontinuities=%u seq_repeat=%u seq_reset=%u seq_last=%u",
		timing->output->name, timing->output->refresh / 1000.0, span,
		schedule_policy_name(schedule->policy), schedule_overlapping(schedule) ? "on" : "off",
		schedule_lead(schedule) / 1e6,
		timing->flips, span > 0 ? timing->flips / span : 0.0,
		timing->composited, timing->failed,
		timing->prepared, timing->promoted, timing->discarded,
		stat_str(&timing->timer_late, late, sizeof(late)), timing->timer_early,
		timing->draw.count, stat_str(&timing->draw, draw, sizeof(draw)),
		timing->empty.count, stat_str(&timing->empty, empty, sizeof(empty)),
		stat_str(&timing->gpu, gpu, sizeof(gpu)), stat_str(&timing->flip, flip, sizeof(flip)),
		timing->seq_steps[0], timing->seq_steps[1], timing->seq_steps[2],
		stat_str(&timing->dispatch, dispatch, sizeof(dispatch)),
		timing->hw_clock ? "hw" : "sw",
		timing->metadata_missing, timing->clock_discontinuities,
		timing->seq_repeat, timing->seq_reset, timing->last_seq);

	reset_span(timing);
	wl_event_source_timer_update(timing->report, REPORT_MS);
	return 0;
}

// ---- hooks ----

void timing_init(struct flame_frame_timing *timing, struct wlr_output *output,
		const struct schedule *schedule, bool enabled) {
	memset(timing, 0, sizeof(*timing));
	timing->output = output;
	timing->schedule = schedule;
	wl_list_init(&timing->present.link);
	if (!enabled) {
		return;
	}
	timing->report = wl_event_loop_add_timer(output->event_loop, handle_report, timing);
	if (!timing->report) {
		wlr_log(WLR_ERROR, "no timer for frame timing on %s, it stays off", output->name);
		return;
	}
	timing->enabled = true;
	reset_span(timing);
	wl_event_source_timer_update(timing->report, REPORT_MS);
	listener_connect(&timing->present, &output->events.present, handle_present);
}

void timing_finish(struct flame_frame_timing *timing) {
	listener_disconnect(&timing->present);
	if (timing->report) {
		wl_event_source_remove(timing->report);
		timing->report = NULL;
	}
	timing->enabled = false;
}

void timing_frame(struct flame_frame_timing *timing, const struct timespec *start,
		const struct timespec *end, bool drew) {
	if (timing->enabled) {
		stat_add(drew ? &timing->draw : &timing->empty, ms_between(start, end));
	}
}

void timing_committed(struct flame_frame_timing *timing, bool ok) {
	if (timing->enabled) {
		ok ? timing->composited++ : timing->failed++;
	}
}

void timing_gpu(struct flame_frame_timing *timing, double max_ms) {
	if (timing->enabled) {
		stat_add(&timing->gpu, max_ms);
	}
}

void timing_successor_prepared(struct flame_frame_timing *timing) {
	if (timing->enabled) {
		timing->prepared++;
	}
}

void timing_successor(struct flame_frame_timing *timing, bool ok) {
	if (timing->enabled) {
		ok ? timing->promoted++ : timing->discarded++;
	}
}

void timing_wake(struct flame_frame_timing *timing, int64_t late_ns) {
	if (!timing->enabled) {
		return;
	}
	if (late_ns < 0) {
		timing->timer_early++;
	} else {
		stat_add(&timing->timer_late, late_ns / 1e6);
	}
}
