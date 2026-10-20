// flamectl: flamewm's ipc from a shell. sends its arguments as one command
// line and prints what comes back:
//   flamectl workspace 3
//   flamectl state
//   flamectl subscribe     (keeps printing the state every time it changes)
// see src/ipc/commands.h for every command
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static void usage(void) {
	fprintf(stderr,
		"usage: flamectl <command> [arg]\n"
		"  subscribe | state | workspace <1-10> | movetoworkspace <1-10>\n"
		"  focus next|prev | fullscreen | close | quit\n");
}

// $FLAMEWM_SOCKET from anything flame started, else worked out the same way
// flame names it, for a shell that wasn't started by flame
static int connect_flame(void) {
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	const char *path = getenv("FLAMEWM_SOCKET");
	if (path) {
		if (strlen(path) >= sizeof(addr.sun_path)) {
			fprintf(stderr, "flamectl: $FLAMEWM_SOCKET is too long\n");
			return -1;
		}
		strcpy(addr.sun_path, path);
	} else {
		const char *runtime = getenv("XDG_RUNTIME_DIR");
		const char *display = getenv("WAYLAND_DISPLAY");
		if (!runtime || !display) {
			fprintf(stderr, "flamectl: no $FLAMEWM_SOCKET, is flamewm running?\n");
			return -1;
		}
		int len = snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/flamewm.%s.sock",
			runtime, display);
		if (len < 0 || (size_t)len >= sizeof(addr.sun_path)) {
			fprintf(stderr, "flamectl: socket path too long\n");
			return -1;
		}
	}

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "flamectl: can't reach flamewm at %s: %s\n", addr.sun_path,
			strerror(errno));
		if (fd >= 0) {
			close(fd);
		}
		return -1;
	}
	return fd;
}

static int send_all(int fd, const char *data, size_t len) {
	while (len > 0) {
		ssize_t sent = send(fd, data, len, MSG_NOSIGNAL);
		if (sent < 0) {
			if (errno == EINTR) {
				continue;
			}
			return -1;
		}
		data += sent;
		len -= (size_t)sent;
	}
	return 0;
}

int main(int argc, char **argv) {
	if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
		usage();
		return argc < 2 ? 1 : 0;
	}

	// the arguments, space separated, as one line
	size_t len = 1;
	for (int i = 1; i < argc; i++) {
		len += strlen(argv[i]) + 1;
	}
	char *line = malloc(len);
	if (!line) {
		return 1;
	}
	line[0] = '\0';
	for (int i = 1; i < argc; i++) {
		if (strchr(argv[i], '\n')) {
			fprintf(stderr, "flamectl: no newlines in arguments\n");
			return 1;
		}
		strcat(line, argv[i]);
		strcat(line, i + 1 < argc ? " " : "\n");
	}

	int fd = connect_flame();
	if (fd < 0) {
		return 1;
	}
	if (send_all(fd, line, strlen(line)) < 0) {
		fprintf(stderr, "flamectl: lost flamewm: %s\n", strerror(errno));
		return 1;
	}
	free(line);

	bool stream = !strcmp(argv[1], "subscribe");
	// replies are one line each. print them as they come, flushed, so a bar
	// reading a pipe sees every change right away
	char buf[65536];
	size_t have = 0;
	for (;;) {
		ssize_t got = recv(fd, buf + have, sizeof(buf) - 1 - have, 0);
		if (got < 0 && errno == EINTR) {
			continue;
		}
		if (got <= 0) {
			if (stream) {
				return 0; // flame went away
			}
			fprintf(stderr, "flamectl: flamewm hung up without answering\n");
			return 1;
		}
		have += (size_t)got;
		buf[have] = '\0';

		char *start = buf, *newline;
		while ((newline = strchr(start, '\n'))) {
			*newline = '\0';
			bool failed = strstr(start, "\"ok\":false") != NULL;
			fprintf(failed ? stderr : stdout, "%s\n", start);
			fflush(failed ? stderr : stdout);
			if (!stream) {
				close(fd);
				return failed ? 1 : 0;
			}
			start = newline + 1;
		}
		have = (size_t)(buf + have - start);
		memmove(buf, start, have);
		if (have == sizeof(buf) - 1) {
			fprintf(stderr, "flamectl: line too long\n");
			return 1;
		}
	}
}
