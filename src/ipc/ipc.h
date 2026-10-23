#ifndef FLAME_IPC_IPC_H
#define FLAME_IPC_IPC_H

#include <stdbool.h>
#include <wayland-server-core.h>

#include "util/strbuf.h"

struct flame_server;

// flame's own socket for bars and scripts, one line at a time: clients send
// plain commands like "workspace 3" (commands.c), flame answers each with a
// json object, and after "subscribe" sends the whole state as json again
// every time it changes (state.c). lives at
// $XDG_RUNTIME_DIR/flamewm.<wayland socket>.sock, everything flame starts
// finds it in $FLAMEWM_SOCKET

struct flame_ipc_client {
	struct wl_list link; // flame_ipc.clients
	struct flame_ipc *ipc;
	int fd;
	struct wl_event_source *source;
	struct strbuf in, out;
	bool subscribed;
	bool media_keys; // the media keys come to it ("media-keys")
	// broken or too far behind, freed once nobody is using it anymore
	bool dead;
	struct {
		// it's being freed: whoever owes it an answer lets go
		struct wl_signal destroy;
	} events;
};

struct flame_ipc {
	struct flame_server *server;
	int fd; // -1 when there's no socket
	char *path;
	struct wl_event_source *source;
	struct wl_list clients; // flame_ipc_client.link
	// the last state sent to subscribers, so nothing gets sent twice
	struct strbuf state;
	struct wl_listener desktop_changed;
};

bool ipc_init(struct flame_ipc *ipc, struct flame_server *server);
// needs the wayland socket name, so it starts with the display. not having
// one isn't fatal, flame just runs without it
void ipc_start(struct flame_ipc *ipc, const char *wayland_socket);
void ipc_finish(struct flame_ipc *ipc);

// queue a line for client, the newline gets added here
void ipc_client_send(struct flame_ipc_client *client, const char *line, size_t len);
// a media key to whoever takes them: false when nobody does
bool ipc_media_key(struct flame_ipc *ipc, const char *name);

#endif
