#include "util/timer.h"

#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

int64_t now_ns(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

static int handle_fd(int fd, uint32_t mask, void *data) {
	struct flame_timer *timer = data;
	uint64_t expirations;
	// drain it, a timerfd stays readable until read
	if (read(fd, &expirations, sizeof(expirations)) != sizeof(expirations)) {
		return 0;
	}
	timer->fired = timer->armed;
	timer->armed = 0;
	timer->fire(timer->data);
	return 0;
}

bool timer_init(struct flame_timer *timer, struct wl_event_loop *loop,
		void (*fire)(void *data), void *data) {
	timer->fire = fire;
	timer->data = data;
	timer->armed = 0;
	timer->fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	if (timer->fd < 0) {
		return false;
	}
	timer->source = wl_event_loop_add_fd(loop, timer->fd, WL_EVENT_READABLE, handle_fd, timer);
	if (!timer->source) {
		close(timer->fd);
		timer->fd = -1;
		return false;
	}
	return true;
}

void timer_finish(struct flame_timer *timer) {
	if (timer->source) {
		wl_event_source_remove(timer->source);
		timer->source = NULL;
	}
	if (timer->fd >= 0) {
		close(timer->fd);
		timer->fd = -1;
	}
}

void timer_arm(struct flame_timer *timer, int64_t deadline) {
	if (timer->fd < 0 || deadline == timer->armed) {
		return;
	}
	// zero disarms, a time in the past fires at once
	int64_t at = deadline > 0 ? deadline : 0;
	struct itimerspec spec = {
		.it_value = {
			.tv_sec = at / 1000000000,
			.tv_nsec = at % 1000000000,
		},
	};
	timerfd_settime(timer->fd, TFD_TIMER_ABSTIME, &spec, NULL);
	timer->armed = deadline > 0 ? deadline : 0;
}
