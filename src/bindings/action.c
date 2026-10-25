#include "bindings/action.h"

#include "config/config.h"
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

void action_fullscreen(struct flame_server *server, const union flame_arg *arg) {
	fullscreen_toggle(&server->desktop);
}

void action_toggle_floating(struct flame_server *server, const union flame_arg *arg) {
	if (server->desktop.focused) {
		floating_toggle(&server->desktop, server->desktop.focused);
	}
}

void action_reload(struct flame_server *server, const union flame_arg *arg) {
	server_reload_config(server);
}

void action_lock(struct flame_server *server, const union flame_arg *arg) {
	const char *command = config_lock_command(server->config);
	if (command) {
		spawn_shell(command);
	}
}

void action_lua(struct flame_server *server, const union flame_arg *arg) {
	config_run_lua_bind(server, arg->i);
}

void action_workspace(struct flame_server *server, const union flame_arg *arg) {
	desktop_switch_workspace(&server->desktop, arg->i);
}

void action_move_to_workspace(struct flame_server *server, const union flame_arg *arg) {
	if (server->desktop.focused) {
		desktop_move_view_to_workspace(&server->desktop, server->desktop.focused, arg->i);
	}
}
