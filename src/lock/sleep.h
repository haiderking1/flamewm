#ifndef FLAME_LOCK_SLEEP_H
#define FLAME_LOCK_SLEEP_H

#include <stdbool.h>
#include <wayland-server-core.h>

// the session locked before the computer sleeps (logind), see sleep.c

struct flame_server;
struct sd_bus;
struct sd_bus_slot;

struct flame_sleep {
	struct flame_server *server;
	struct sd_bus *bus; // the system's, NULL when flame isn't the session
	struct sd_bus_slot *slot;
	struct wl_event_source *source, *timer, *give_up;
	int inhibitor; // logind waits for us while it's open, -1 for none
	bool waiting; // going to sleep, the lock's coming
};

bool sleep_init(struct flame_sleep *sleep, struct flame_server *server);
void sleep_finish(struct flame_sleep *sleep);
// every screen shows it locked: sleep needn't wait any more
void sleep_locked(struct flame_sleep *sleep);

#endif
