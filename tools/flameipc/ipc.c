#include "ipc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int ipc_connect(void) {
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	const char *path = getenv("FLAMEWM_SOCKET");
	if (path) {
		snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	} else {
		const char *runtime = getenv("XDG_RUNTIME_DIR"), *display = getenv("WAYLAND_DISPLAY");
		if (!runtime || !display) {
			return -1;
		}
		snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/flamewm.%s.sock", runtime, display);
	}
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		if (fd >= 0) {
			close(fd);
		}
		return -1;
	}
	return fd;
}

char *ipc_call(const char *command) {
	int fd = ipc_connect();
	if (fd < 0) {
		return NULL;
	}
	size_t len = strlen(command);
	if (write(fd, command, len) != (ssize_t)len || write(fd, "\n", 1) != 1) {
		close(fd);
		return NULL;
	}
	size_t cap = 4096, have = 0;
	char *buf = malloc(cap);
	while (buf) {
		if (have + 1 >= cap) {
			cap *= 2;
			char *grown = realloc(buf, cap);
			if (!grown) {
				free(buf);
				buf = NULL;
				break;
			}
			buf = grown;
		}
		ssize_t got = read(fd, buf + have, cap - have - 1);
		if (got < 0 && errno == EINTR) {
			continue;
		}
		if (got <= 0) {
			break;
		}
		have += (size_t)got;
		buf[have] = '\0';
		if (memchr(buf, '\n', have)) {
			break;
		}
	}
	close(fd);
	if (buf) {
		buf[have] = '\0';
		char *nl = strchr(buf, '\n');
		if (nl) {
			*nl = '\0';
		}
	}
	return buf;
}

