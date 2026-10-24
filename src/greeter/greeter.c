// syscall (pidfd_open) isn't posix
#define _GNU_SOURCE
#include "greeter/greeter.h"

#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wlr/util/log.h>

#include "core/server.h"
#include "util/spawn.h"

static int handle_exited(int fd, uint32_t mask, void *data) {
	struct flame_greeter *greeter = data;
	int status = 0;
	waitpid(greeter->pid, &status, 0);
	wlr_log(WLR_INFO, "greeter: done (%s %d), flame goes too",
		WIFEXITED(status) ? "exit" : "signal",
		WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status));
	wl_event_source_remove(greeter->exited);
	greeter->exited = NULL;
	close(greeter->pidfd);
	greeter->pidfd = -1;
	server_terminate(greeter->server);
	return 0;
}

bool greeter_start(struct flame_greeter *greeter, struct flame_server *server,
		const char *command) {
	greeter->server = server;
	greeter->pidfd = -1;
	const char *const argv[] = {"/bin/sh", "-c", command, NULL};
	greeter->pid = spawn_child(argv);
	if (greeter->pid < 0) {
		greeter->pid = 0;
		return false;
	}
	greeter->pidfd = (int)syscall(SYS_pidfd_open, greeter->pid, 0);
	if (greeter->pidfd < 0) {
		wlr_log_errno(WLR_ERROR, "greeter: can't watch it");
		return false;
	}
	greeter->exited = wl_event_loop_add_fd(server->loop, greeter->pidfd, WL_EVENT_READABLE,
		handle_exited, greeter);
	wlr_log(WLR_INFO, "greeter: %s", command);
	return greeter->exited != NULL;
}

void greeter_finish(struct flame_greeter *greeter) {
	if (greeter->exited) {
		wl_event_source_remove(greeter->exited);
		greeter->exited = NULL;
	}
	if (greeter->pidfd >= 0) {
		close(greeter->pidfd);
		greeter->pidfd = -1;
	}
}

bool greeter_active(const struct flame_greeter *greeter) {
	return greeter->pid > 0;
}
