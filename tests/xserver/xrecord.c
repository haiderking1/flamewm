// records what X clients send, for the fuzzer's corpus: listens as display
// :N, passes everything on to $DISPLAY, writes each connection's requests
// (setup included) to DIR/conn-K.bin. fds don't get passed, apps fall back
// usage: xrecord N DIR   (DISPLAY is where it all goes)
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define MAX 64

static int listener;
struct conn {
	int client, server, log;
};
static struct conn conns[MAX];
static int conns_len;

static int unix_socket(const char *path, bool listen_on) {
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		return -1;
	}
	if (listen_on) {
		unlink(path);
		if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 16) < 0) {
			close(fd);
			return -1;
		}
	} else if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static bool pump(int from, int to, int log) {
	char buf[65536];
	ssize_t n = read(from, buf, sizeof(buf));
	if (n <= 0) {
		return false;
	}
	if (log >= 0 && write(log, buf, (size_t)n) != n) {
		return false;
	}
	for (ssize_t done = 0; done < n;) {
		ssize_t w = write(to, buf + done, (size_t)(n - done));
		if (w <= 0) {
			return false;
		}
		done += w;
	}
	return true;
}

int main(int argc, char **argv) {
	const char *target = getenv("DISPLAY");
	if (argc < 3 || !target) {
		fprintf(stderr, "usage: xrecord N DIR, with DISPLAY set\n");
		return 2;
	}
	signal(SIGPIPE, SIG_IGN);
	char path[108], target_path[108];
	snprintf(path, sizeof(path), "/tmp/.X11-unix/X%s", argv[1]);
	snprintf(target_path, sizeof(target_path), "/tmp/.X11-unix/X%s", target + 1);
	listener = unix_socket(path, true);
	if (listener < 0) {
		perror("listen");
		return 1;
	}
	int count = 0;
	for (;;) {
		struct pollfd fds[1 + 2 * MAX];
		fds[0] = (struct pollfd){listener, POLLIN, 0};
		for (int i = 0; i < conns_len; i++) {
			fds[1 + 2 * i] = (struct pollfd){conns[i].client, POLLIN, 0};
			fds[2 + 2 * i] = (struct pollfd){conns[i].server, POLLIN, 0};
		}
		if (poll(fds, (nfds_t)(1 + 2 * conns_len), -1) < 0 && errno != EINTR) {
			return 1;
		}
		if ((fds[0].revents & POLLIN) && conns_len < MAX) {
			int client = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
			int server = client >= 0 ? unix_socket(target_path, false) : -1;
			if (server >= 0) {
				char name[256];
				snprintf(name, sizeof(name), "%s/conn-%d.bin", argv[2], count++);
				int log = open(name, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
				conns[conns_len++] = (struct conn){client, server, log};
			} else if (client >= 0) {
				close(client);
			}
		}
		for (int i = conns_len - 1; i >= 0; i--) {
			bool ok = true;
			if (fds[1 + 2 * i].revents & (POLLIN | POLLHUP | POLLERR)) {
				ok = pump(conns[i].client, conns[i].server, conns[i].log);
			}
			if (ok && (fds[2 + 2 * i].revents & (POLLIN | POLLHUP | POLLERR))) {
				ok = pump(conns[i].server, conns[i].client, -1);
			}
			if (!ok) {
				close(conns[i].client);
				close(conns[i].server);
				if (conns[i].log >= 0) {
					close(conns[i].log);
				}
				conns[i] = conns[--conns_len];
			}
		}
	}
}
