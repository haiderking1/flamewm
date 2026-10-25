#include "util/spawn.h"

#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wlr/util/log.h>

// the folder flame's own binary is in, into dir. false when it can't tell
static bool flame_dir(char *dir, size_t size) {
	ssize_t len = readlink("/proc/self/exe", dir, size - 1);
	if (len <= 0) {
		return false;
	}
	dir[len] = '\0';
	char *slash = strrchr(dir, '/');
	if (!slash) {
		return false;
	}
	*slash = '\0';
	return true;
}

// a bare name next to flame's own binary first: flame's tools (flamectl,
// flamesettings) run from wherever flame does, installed or a build dir.
// returns only when there's none
static void exec_near_flame(const char *const argv[]) {
	char path[PATH_MAX];
	if (strchr(argv[0], '/') || !flame_dir(path, sizeof(path))
			|| strlen(path) + 2 + strlen(argv[0]) > sizeof(path)) {
		return;
	}
	strcat(path, "/");
	strcat(path, argv[0]);
	if (access(path, X_OK) == 0) {
		execv(path, (char *const *)argv);
	}
}

// what's started finds flame's tools the same way, whatever it runs
// (autostart's shell lines, a terminal typing flamectl): flame's folder
// first on its PATH, when it isn't there already
static void path_near_flame(void) {
	char dir[PATH_MAX];
	if (!flame_dir(dir, sizeof(dir))) {
		return;
	}
	const char *path = getenv("PATH");
	size_t len = strlen(dir);
	for (const char *at = path; at && *at;) {
		const char *end = strchr(at, ':');
		size_t n = end ? (size_t)(end - at) : strlen(at);
		if (n == len && !strncmp(at, dir, len)) {
			return; // there already
		}
		at = end ? end + 1 : at + n;
	}
	char joined[PATH_MAX * 2];
	snprintf(joined, sizeof(joined), "%s%s%s", dir, path && *path ? ":" : "", path ? path : "");
	setenv("PATH", joined, 1);
}

bool spawn_findable(const char *name) {
	char path[PATH_MAX];
	if (flame_dir(path, sizeof(path)) && strlen(path) + 2 + strlen(name) <= sizeof(path)) {
		strcat(path, "/");
		strcat(path, name);
		if (access(path, X_OK) == 0) {
			return true;
		}
	}
	const char *dirs = getenv("PATH");
	for (const char *at = dirs; at && *at;) {
		const char *end = strchr(at, ':');
		int n = end ? (int)(end - at) : (int)strlen(at);
		snprintf(path, sizeof(path), "%.*s/%s", n, at, name);
		if (n > 0 && access(path, X_OK) == 0) {
			return true;
		}
		at = end ? end + 1 : at + n;
	}
	return false;
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
			path_near_flame();
			exec_near_flame(argv);
			execvp(argv[0], (char *const *)argv);
			_exit(127);
		}
		_exit(child < 0 ? 1 : 0);
	}
	waitpid(pid, NULL, 0);
}

pid_t spawn_reading(const char *const argv[], int *out) {
	int pipe_fds[2];
	if (pipe(pipe_fds) < 0) {
		return -1;
	}
	// flame's other fds aren't the child's business, nor is this one's copy
	fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC);
	fcntl(pipe_fds[1], F_SETFD, FD_CLOEXEC);
	pid_t pid = fork();
	if (pid < 0) {
		wlr_log_errno(WLR_ERROR, "fork failed for %s", argv[0]);
		close(pipe_fds[0]);
		close(pipe_fds[1]);
		return -1;
	}
	if (pid == 0) {
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, NULL);
		signal(SIGPIPE, SIG_DFL);
		setpriority(PRIO_PROCESS, 0, 0);
		setsid();
		dup2(pipe_fds[1], STDOUT_FILENO); // dup2 clears close-on-exec on the copy
		path_near_flame();
		exec_near_flame(argv);
		execvp(argv[0], (char *const *)argv);
		_exit(127);
	}
	close(pipe_fds[1]);
	fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK);
	*out = pipe_fds[0];
	return pid;
}

pid_t spawn_child(const char *const argv[]) {
	pid_t pid = fork();
	if (pid < 0) {
		wlr_log_errno(WLR_ERROR, "fork failed for %s", argv[0]);
		return -1;
	}
	if (pid == 0) {
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, NULL);
		signal(SIGPIPE, SIG_DFL);
		setpriority(PRIO_PROCESS, 0, 0);
		setsid();
		path_near_flame();
		exec_near_flame(argv);
		execvp(argv[0], (char *const *)argv);
		_exit(127);
	}
	return pid;
}

void spawn_shell(const char *command) {
	const char *const argv[] = {"/bin/sh", "-c", command, NULL};
	spawn(argv);
}
