#ifndef FLAME_GREETER_GREETER_H
#define FLAME_GREETER_GREETER_H

#include <stdbool.h>
#include <sys/types.h>
#include <wayland-server-core.h>

struct flame_server;

// flame as the login screen (greetd's greeter, flamewm -g flamegreet): the
// greeter's all it runs. no autostart, no binds, no X server, and flame's
// gone the moment the greeter is: greetd starts what the user logged into
// only once its greeter's out of the way

struct flame_greeter {
	struct flame_server *server;
	pid_t pid; // 0: flame's a session like any other
	struct wl_event_source *exited;
	int pidfd;
};

// command started, flame quits once it exits. false: it couldn't be
bool greeter_start(struct flame_greeter *greeter, struct flame_server *server,
	const char *command);
void greeter_finish(struct flame_greeter *greeter);
bool greeter_active(const struct flame_greeter *greeter);

#endif
