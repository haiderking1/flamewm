#ifndef FLAME_UTIL_TIMER_H
#define FLAME_UTIL_TIMER_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

// a one-shot timer with nanosecond deadlines on CLOCK_MONOTONIC.
// wl_event_loop timers only do milliseconds, a 165hz frame is 6.06ms
struct flame_timer {
	int fd;
	struct wl_event_source *source;
	void (*fire)(void *data);
	void *data;
	int64_t armed; // deadline, 0 when not armed
	int64_t fired; // the deadline that fired last, for seeing how late it was
};

bool timer_init(struct flame_timer *timer, struct wl_event_loop *loop,
	void (*fire)(void *data), void *data);
void timer_finish(struct flame_timer *timer);
// absolute CLOCK_MONOTONIC ns, 0 disarms. a deadline in the past fires right away
void timer_arm(struct flame_timer *timer, int64_t deadline);

int64_t now_ns(void);

#endif
