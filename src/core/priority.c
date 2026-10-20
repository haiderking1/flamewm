#define _GNU_SOURCE
#include "core/priority.h"

#include <errno.h>
#include <sched.h>
#include <string.h>
#include <sys/resource.h>
#include <wlr/util/log.h>

void priority_raise(void) {
	struct sched_param param = {.sched_priority = sched_get_priority_min(SCHED_RR)};
	if (sched_setscheduler(0, SCHED_RR | SCHED_RESET_ON_FORK, &param) == 0) {
		wlr_log(WLR_INFO, "realtime scheduling on (round robin, priority %d)",
			param.sched_priority);
		return;
	}
	int rr_errno = errno;
	// RLIMIT_NICE says how far down nice may go: 20 - limit
	struct rlimit limit;
	int nice = 0;
	if (getrlimit(RLIMIT_NICE, &limit) == 0) {
		nice = limit.rlim_cur == RLIM_INFINITY || limit.rlim_cur >= 40
			? -20 : 20 - (int)limit.rlim_cur;
	}
	if (nice < 0 && setpriority(PRIO_PROCESS, 0, nice) == 0) {
		wlr_log(WLR_INFO, "no realtime scheduling (%s), running at nice %d instead",
			strerror(rr_errno), nice);
		return;
	}
	wlr_log(WLR_INFO, "no realtime scheduling (%s), frames may slip under load",
		strerror(rr_errno));
}
