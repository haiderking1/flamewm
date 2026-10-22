#include "util/spawn.h"

#include <limits.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wlr/util/log.h>

// a bare name next to flame's own binary first: flame's tools (flamectl,
// flamesettings) run from wherever flame does, installed or a build dir.
// returns only when there's none
static void exec_near_flame(const char *const argv[]) {
	if (strchr(argv[0], '/')) {
		return;
	}
	char path[PATH_MAX];
	ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
	if (len <= 0) {
		return;
	}
	path[len] = '\0';
	char *slash = strrchr(path, '/');
	if (!slash || (size_t)(slash - path) + 2 + strlen(argv[0]) > sizeof(path)) {
		return;
	}
	strcpy(slash + 1, argv[0]);
	if (access(path, X_OK) == 0) {
		execv(path, (char *const *)argv);
	}
}

void spawn(const char *const argv[]) {
	pid_t pid = fork();
	if (pid < 0) {
		wlr_log_errno(WLR_ERROR, "fork failed for %s", argv[0]);
		return;
	}
	if (pid == 0) {
		// the event loop blocks the signals it watches through signalfd,
		// children shouldn't inherit that or our ignored SIGPIPE
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, NULL);
		signal(SIGPIPE, SIG_DFL);
		// realtime doesn't carry over (SCHED_RESET_ON_FORK), a raised nice
		// would, apps run like any other process
		setpriority(PRIO_PROCESS, 0, 0);
		setsid();

		pid_t child = fork();
		if (child == 0) {
			exec_near_flame(argv);
			execvp(argv[0], (char *const *)argv);
			_exit(127);
		}
		_exit(child < 0 ? 1 : 0);
	}
	waitpid(pid, NULL, 0);
}

void spawn_shell(const char *command) {
	const char *const argv[] = {"/bin/sh", "-c", command, NULL};
	spawn(argv);
}
