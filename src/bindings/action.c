#include "bindings/action.h"

#include "core/server.h"
#include "util/spawn.h"

void action_spawn(struct flame_server *server, const union flame_arg *arg) {
	spawn(arg->argv);
}

void action_quit(struct flame_server *server, const union flame_arg *arg) {
	server_terminate(server);
}

void action_close(struct flame_server *server, const union flame_arg *arg) {
	if (server->desktop.focused) {
		view_close(server->desktop.focused);
	}
}

void action_focus(struct flame_server *server, const union flame_arg *arg) {
	focus_cycle(&server->desktop, arg->i);
}

void action_workspace(struct flame_server *server, const union flame_arg *arg) {
	desktop_switch_workspace(&server->desktop, arg->i);
}

void action_move_to_workspace(struct flame_server *server, const union flame_arg *arg) {
	if (server->desktop.focused) {
		desktop_move_view_to_workspace(&server->desktop, server->desktop.focused, arg->i);
	}
}
