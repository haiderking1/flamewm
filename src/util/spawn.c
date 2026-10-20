#include "util/spawn.h"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wlr/util/log.h>

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
		setsid();

		pid_t child = fork();
		if (child == 0) {
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
