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
		"  focus next|prev | fullscreen | close | quit\n"
		"  share-pick [--dmenu]  (what to share, for xdg-desktop-portal-wlr)\n");
}

// xdg-desktop-portal-wlr's dmenu chooser: what the app asked for comes in on
// stdin, a line a source ("Monitor: NAME DESCRIPTION", "Window: TITLE (ID)"),
// the pick goes back as one of those lines exactly. the picker's told which
// kinds there are, so a screen asked for opens on the screen
struct portal_list {
	char **lines;
	size_t len;
	bool screens, windows;
};

static bool read_portal_list(struct portal_list *list) {
	*list = (struct portal_list){0};
	size_t cap = 0;
	char *line = NULL;
	size_t size = 0;
	ssize_t got;
	while ((got = getline(&line, &size, stdin)) >= 0) {
		if (got > 0 && line[got - 1] == '\n') {
			line[got - 1] = '\0';
		}
		if (!strncmp(line, "Monitor: ", 9)) {
			list->screens = true;
		} else if (!strncmp(line, "Window: ", 8)) {
			list->windows = true;
		} else {
			continue;
		}
		if (list->len == cap) {
			cap = cap ? cap * 2 : 16;
			char **grown = realloc(list->lines, cap * sizeof(*grown));
			if (!grown) {
				return false;
			}
			list->lines = grown;
		}
		list->lines[list->len++] = strdup(line);
	}
	free(line);
	return true;
}

// the portal's line for flame's answer: "Monitor: NAME" is the line for
// that output, "Window: ID" the one ending in (ID). NULL: none
static const char *portal_line(const struct portal_list *list, const char *answer) {
	size_t len = strlen(answer);
	for (size_t i = 0; i < list->len; i++) {
		const char *line = list->lines[i];
		if (!strncmp(answer, "Monitor: ", 9)) {
			if (!strncmp(line, answer, len) && (line[len] == ' ' || line[len] == '\0')) {
				return line;
			}
		} else if (!strncmp(answer, "Window: ", 8)) {
			const char *id = answer + 8;
			size_t id_len = strlen(id), line_len = strlen(line);
			if (line_len >= id_len + 2 && line[line_len - 1] == ')'
					&& line[line_len - id_len - 2] == '('
					&& !strncmp(line + line_len - id_len - 1, id, id_len)) {
				return line;
			}
		}
	}
	return NULL;
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

	struct portal_list portal = {0};
	bool dmenu = !strcmp(argv[1], "share-pick") && argc == 3 && !strcmp(argv[2], "--dmenu");
	if (dmenu) {
		if (!read_portal_list(&portal)) {
			return 1;
		}
		// asked for: share-pick screen window, or one of them
		static char screen[] = "screen", window[] = "window";
		static char *kinds[4];
		int n = 0;
		kinds[n++] = argv[0];
		kinds[n++] = argv[1];
		if (portal.screens) {
			kinds[n++] = screen;
		}
		if (portal.windows) {
			kinds[n++] = window;
		}
		argc = n;
		argv = kinds;
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
			const char *pick = dmenu && !failed ? portal_line(&portal, start) : NULL;
			fprintf(failed ? stderr : stdout, "%s\n", pick ? pick : start);
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
