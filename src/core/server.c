#include "core/server.h"

#include <stdlib.h>
#include <wlr/util/log.h>

#include "protocols/globals.h"
#include "util/spawn.h"

bool server_init(struct flame_server *server, const struct flame_config *config) {
	server->config = config;
	server->display = wl_display_create();
	if (!server->display) {
		wlr_log(WLR_ERROR, "failed to create wl_display");
		return false;
	}
	server->loop = wl_display_get_event_loop(server->display);

	// order matters: each step only uses what came before it
	return signals_init(&server->signals, server)
		&& backend_init(&server->backend, server->loop)
		&& outputs_init(&server->outputs, server)
		&& render_init(&server->render, server)
		&& globals_init(server)
		&& desktop_init(&server->desktop, server)
		&& input_init(&server->input, server)
		&& xdg_init(&server->xdg, server)
		&& layers_init(&server->layers, server)
		&& ipc_init(&server->ipc, server)
		&& ext_workspaces_init(&server->ext_workspaces, server);
}

bool server_start(struct flame_server *server, const char *startup_cmd) {
	server->socket = wl_display_add_socket_auto(server->display);
	if (!server->socket) {
		wlr_log(WLR_ERROR, "failed to open a wayland socket");
		return false;
	}

	// read the host's WAYLAND_DISPLAY (nested) before we overwrite it
	if (!backend_start(&server->backend)) {
		return false;
	}

	setenv("WAYLAND_DISPLAY", server->socket, true);
	setenv("XDG_CURRENT_DESKTOP", "flamewm", true);
	// started from a tty this says "tty", and chromium (helium, brave...)
	// takes anything but "wayland" as go x11, finds no X server and quits
	setenv("XDG_SESSION_TYPE", "wayland", true);
	// before anything gets started, they all inherit $FLAMEWM_SOCKET
	ipc_start(&server->ipc, server->socket);

	wlr_log(WLR_INFO, "flamewm running on WAYLAND_DISPLAY=%s", server->socket);
	if (startup_cmd) {
		spawn_shell(startup_cmd);
	}
	return true;
}

void server_run(struct flame_server *server) {
	wl_display_run(server->display);
}

void server_terminate(struct flame_server *server) {
	wl_display_terminate(server->display);
}

void server_finish(struct flame_server *server) {
	wl_display_destroy_clients(server->display);
	// stop listening to the desktop before it goes
	ipc_finish(&server->ipc);
	ext_workspaces_finish(&server->ext_workspaces);
	desktop_finish(&server->desktop);

	// unhook from wlroots objects that are about to die
	xdg_finish(&server->xdg);
	layers_finish(&server->layers);
	input_finish(&server->input);
	outputs_finish(&server->outputs);

	// outputs and input devices die here, their own destroy handlers clean
	// up after them while the scene is still alive
	backend_finish(&server->backend);
	signals_finish(&server->signals);

	render_unhook(&server->render);
	// globals, seat and output layout go with the display
	wl_display_destroy(server->display);

	// scene after the display, its surfaces are gone by then. renderer dead
	// last since everything above may still reference it
	render_finish(&server->render);
}
