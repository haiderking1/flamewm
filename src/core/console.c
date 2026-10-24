#include "core/console.h"

#include <fcntl.h>
#include <linux/kd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static char tty[64]; // the console flame was started on, "" for none
static bool cursor_back;

void console_quiet(bool greeter) {
	int mode;
	if (!isatty(STDERR_FILENO) || ioctl(STDERR_FILENO, KDGETMODE, &mode) < 0) {
		return; // a terminal's, not a console
	}
	const char *name = ttyname(STDERR_FILENO);
	snprintf(tty, sizeof(tty), "%s", name ? name : "");
	cursor_back = !greeter;
	// black, no cursor, nothing of what was there (boot, the last session)
	static const char clear[] = "\033[H\033[2J\033[3J\033[?25l";
	if (write(STDERR_FILENO, clear, sizeof(clear) - 1) < 0) {
		return;
	}
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	char path[4096];
	if (runtime && *runtime) {
		snprintf(path, sizeof(path), "%s/flamewm%s.log", runtime, greeter ? "-greeter" : "");
	} else {
		snprintf(path, sizeof(path), "/tmp/flamewm%s-%d.log", greeter ? "-greeter" : "",
			(int)getuid());
	}
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0) {
		return;
	}
	// flame's and everything it starts
	dup2(fd, STDOUT_FILENO);
	dup2(fd, STDERR_FILENO);
	close(fd);
}

void console_restore(void) {
	if (!*tty || !cursor_back) {
		return;
	}
	int fd = open(tty, O_WRONLY | O_NOCTTY | O_CLOEXEC);
	if (fd >= 0) {
		static const char show[] = "\033[?25h";
		if (write(fd, show, sizeof(show) - 1) < 0) {
			// it's gone, nothing to show it on
		}
		close(fd);
	}
}
