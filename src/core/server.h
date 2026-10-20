#ifndef FLAME_CORE_SERVER_H
#define FLAME_CORE_SERVER_H

#include <stdbool.h>
#include <wayland-server-core.h>

#include "anim/anim.h"
#include "backend/backend.h"
#include "core/signals.h"
#include "desktop/desktop.h"
#include "input/input.h"
#include "ipc/ipc.h"
#include "layers/layers.h"
#include "output/output.h"
#include "protocols/workspaces.h"
#include "render/render.h"
#include "shell/xdg.h"
#include "xbridge/xbridge.h"
#include "xselection/xselection.h"
#include "xshell/xshell.h"

struct flame_config;

// the root everything hangs off. each subsystem owns its own state, this just
// holds them together and decides the order they come up and go down in
struct flame_server {
	struct wl_display *display;
	struct wl_event_loop *loop;
	const char *socket;
	const struct flame_config *config;
	const char *config_path; // -c, NULL for the usual place
	struct wl_event_source *reload_idle;

	struct flame_signals signals;
	struct flame_anims anims;
	struct flame_backend backend;
	struct flame_outputs outputs;
	struct flame_render render;
	struct flame_desktop desktop;
	struct flame_input input;
	struct flame_xdg xdg;
	struct flame_layers layers;
	struct flame_ipc ipc;
	struct flame_xbridge xbridge;
	struct flame_xshell xshell;
	struct flame_xselection xselection;
	struct flame_ext_workspaces ext_workspaces;
};

bool server_init(struct flame_server *server, const struct flame_config *config);
bool server_start(struct flame_server *server, const char *startup_cmd);
void server_run(struct flame_server *server);
void server_terminate(struct flame_server *server);
void server_finish(struct flame_server *server);
// init.lua again, soon: not from inside the lua that asked for it
void server_reload_config(struct flame_server *server);

#endif
