// accept4
#define _GNU_SOURCE
#include "ipc/ipc.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wlr/util/log.h>

#include "core/server.h"
#include "ipc/commands.h"
#include "ipc/state.h"
#include "util/listener.h"

// longer than this without a newline isn't a command, it's garbage
#define MAX_LINE 4096
// a subscriber this far behind isn't reading anymore
#define MAX_PENDING (1 << 20)

static void client_free(struct flame_ipc_client *client) {
	wl_signal_emit_mutable(&client->events.destroy, client);
	wl_event_source_remove(client->source);
	close(client->fd);
	wl_list_remove(&client->link);
	strbuf_finish(&client->in);
	strbuf_finish(&client->out);
	free(client);
}

static void flush(struct flame_ipc_client *client) {
	while (client->out.len > 0) {
		// a bar that died mid write must not take flame down with SIGPIPE
		ssize_t sent = send(client->fd, client->out.data, client->out.len,
			MSG_NOSIGNAL | MSG_DONTWAIT);
		if (sent < 0) {
			if (errno == EINTR) {
				continue;
			}
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				client->dead = true;
				return;
			}
			break;
		}
		strbuf_consume(&client->out, (size_t)sent);
	}
	// woken up again once there's room for the rest
	wl_event_source_fd_update(client->source,
		WL_EVENT_READABLE | (client->out.len ? WL_EVENT_WRITABLE : 0));
}

void ipc_client_send(struct flame_ipc_client *client, const char *line, size_t len) {
	if (client->dead) {
		return;
	}
	strbuf_append(&client->out, line, len);
	strbuf_append(&client->out, "\n", 1);
	if (client->out.failed || client->out.len > MAX_PENDING) {
		wlr_log(WLR_INFO, "ipc client isn't reading, dropping it");
		client->dead = true;
		return;
	}
	flush(client);
}

static void handle_lines(struct flame_ipc_client *client) {
	size_t done = 0;
	char *newline;
	while (!client->dead
			&& (newline = memchr(client->in.data + done, '\n', client->in.len - done))) {
		*newline = '\0';
		char *line = client->in.data + done;
		done = (size_t)(newline - client->in.data) + 1;
		ipc_command(client, line);
	}
	strbuf_consume(&client->in, done);
}

static void read_client(struct flame_ipc_client *client) {
	char chunk[4096];
	ssize_t got = recv(client->fd, chunk, sizeof(chunk), MSG_DONTWAIT);
	if (got < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
			client->dead = true;
		}
		return;
	}
	if (got == 0) {
		client->dead = true; // hung up
		return;
	}
	strbuf_append(&client->in, chunk, (size_t)got);
	handle_lines(client);
	if (client->in.failed || client->in.len > MAX_LINE) {
		client->dead = true;
	}
}

static int handle_client(int fd, uint32_t mask, void *data) {
	struct flame_ipc_client *client = data;
	// read before looking at hangups, a command can come with the hangup
	if (mask & WL_EVENT_READABLE) {
		read_client(client);
	} else if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		client->dead = true;
	}
	if (!client->dead && (mask & WL_EVENT_WRITABLE)) {
		flush(client);
	}
	// commands only ever mark it, this is the one place it goes away
	if (client->dead) {
		client_free(client);
	}
	return 0;
}

static int handle_accept(int fd, uint32_t mask, void *data) {
	struct flame_ipc *ipc = data;
	int client_fd = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
	if (client_fd < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
			wlr_log_errno(WLR_ERROR, "ipc accept failed");
		}
		return 0;
	}
	struct flame_ipc_client *client = calloc(1, sizeof(*client));
	if (!client) {
		wlr_log(WLR_ERROR, "out of memory");
		close(client_fd);
		return 0;
	}
	client->ipc = ipc;
	client->fd = client_fd;
	strbuf_init(&client->in);
	strbuf_init(&client->out);
	wl_signal_init(&client->events.destroy);
	client->source = wl_event_loop_add_fd(ipc->server->loop, client_fd,
		WL_EVENT_READABLE, handle_client, client);
	if (!client->source) {
		wlr_log(WLR_ERROR, "failed to watch an ipc client");
		close(client_fd);
		free(client);
		return 0;
	}
	wl_list_insert(&ipc->clients, &client->link);
	return 0;
}

static void handle_desktop_changed(struct wl_listener *listener, void *data) {
	struct flame_ipc *ipc = wl_container_of(listener, ipc, desktop_changed);

	struct strbuf state;
	strbuf_init(&state);
	ipc_state_write(&ipc->server->desktop, &state);
	// focus moving inside the same window, a title set to what it was...
	if (state.failed || strbuf_equal(&state, &ipc->state)) {
		strbuf_finish(&state);
		return;
	}
	strbuf_finish(&ipc->state);
	ipc->state = state;

	struct flame_ipc_client *client, *tmp;
	wl_list_for_each_safe(client, tmp, &ipc->clients, link) {
		if (client->subscribed) {
			ipc_client_send(client, ipc->state.data, ipc->state.len);
		}
		if (client->dead) {
			client_free(client);
		}
	}
}

bool ipc_media_key(struct flame_ipc *ipc, const char *name) {
	char line[128];
	int len = snprintf(line, sizeof(line), "{\"event\":\"key\",\"key\":\"%s\"}", name);
	bool taken = false;
	struct flame_ipc_client *client, *tmp;
	wl_list_for_each_safe(client, tmp, &ipc->clients, link) {
		if (client->media_keys && !client->dead) {
			ipc_client_send(client, line, (size_t)len);
			taken |= !client->dead;
		}
		if (client->dead) {
			client_free(client);
		}
	}
	return taken;
}

bool ipc_init(struct flame_ipc *ipc, struct flame_server *server) {
	ipc->server = server;
	ipc->fd = -1;
	wl_list_init(&ipc->clients);
	strbuf_init(&ipc->state);
	wl_list_init(&ipc->desktop_changed.link);
	return true;
}

void ipc_start(struct flame_ipc *ipc, const char *wayland_socket) {
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (!runtime) {
		wlr_log(WLR_ERROR, "no XDG_RUNTIME_DIR, running without the ipc socket");
		return;
	}
	struct strbuf path;
	strbuf_init(&path);
	strbuf_printf(&path, "%s/flamewm.%s.sock", runtime, wayland_socket);
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	if (path.failed || path.len >= sizeof(addr.sun_path)) {
		wlr_log(WLR_ERROR, "ipc socket path too long, running without it");
		strbuf_finish(&path);
		return;
	}
	memcpy(addr.sun_path, path.data, path.len + 1);

	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		wlr_log_errno(WLR_ERROR, "failed to create the ipc socket");
		strbuf_finish(&path);
		return;
	}
	// our wayland socket name is locked to us, so a file with this name is
	// left over from a flame that crashed
	unlink(path.data);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 16) < 0) {
		wlr_log_errno(WLR_ERROR, "failed to listen on %s", path.data);
		close(fd);
		strbuf_finish(&path);
		return;
	}
	ipc->source = wl_event_loop_add_fd(ipc->server->loop, fd, WL_EVENT_READABLE,
		handle_accept, ipc);
	if (!ipc->source) {
		wlr_log(WLR_ERROR, "failed to watch the ipc socket");
		close(fd);
		unlink(path.data);
		strbuf_finish(&path);
		return;
	}
	ipc->fd = fd;
	ipc->path = path.data; // ours now, freed in finish
	setenv("FLAMEWM_SOCKET", ipc->path, true);
	listener_connect(&ipc->desktop_changed, &ipc->server->desktop.events.changed,
		handle_desktop_changed);
	wlr_log(WLR_INFO, "ipc listening on %s", ipc->path);
}

void ipc_finish(struct flame_ipc *ipc) {
	listener_disconnect(&ipc->desktop_changed);
	struct flame_ipc_client *client, *tmp;
	wl_list_for_each_safe(client, tmp, &ipc->clients, link) {
		client_free(client);
	}
	if (ipc->source) {
		wl_event_source_remove(ipc->source);
		ipc->source = NULL;
	}
	if (ipc->fd >= 0) {
		close(ipc->fd);
		ipc->fd = -1;
	}
	if (ipc->path) {
		unlink(ipc->path);
		free(ipc->path);
		ipc->path = NULL;
	}
	strbuf_finish(&ipc->state);
}
