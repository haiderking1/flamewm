#ifndef FLAME_CORE_SIGNALS_H
#define FLAME_CORE_SIGNALS_H

#include <stdbool.h>
#include <wayland-server-core.h>

struct flame_server;

// unix signals, not wl_signals
struct flame_signals {
	struct wl_event_source *sigint;
	struct wl_event_source *sigterm;
};

bool signals_init(struct flame_signals *signals, struct flame_server *server);
void signals_finish(struct flame_signals *signals);

#endif
