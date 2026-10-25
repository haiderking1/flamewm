#ifndef FLAME_CORE_SERVER_H
#define FLAME_CORE_SERVER_H

#include <stdbool.h>
#include <wayland-server-core.h>

#include "anim/anim.h"
#include "backend/backend.h"
#include "capture/capture.h"
#include "eyecandy/eyecandy.h"
#include "greeter/greeter.h"
#include "idle/idle.h"
#include "config/watch.h"
#include "core/signals.h"
#include "desktop/desktop.h"
#include "input/input.h"
#include "ipc/ipc.h"
#include "layers/layers.h"
#include "lock/lock.h"
#include "picker/picker.h"
#include "portal/portal.h"
#include "screencast/screencast.h"
#include "output/output.h"
#include "protocols/workspaces.h"
#include "render/render.h"
#include "shell/xdg.h"
#include "xbridge/xbridge.h"
#include "xselection/xselection.h"
#include "xshell/xshell.h"
#include "wallpaper/wallpaper.h"

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
	struct flame_config_watch config_watch; // saved: reloaded
	bool terminating; // tearing down: nothing animates its way out

	struct flame_signals signals;
	struct flame_anims anims;
	struct flame_backend backend;
	struct flame_outputs outputs;
	struct flame_render render;
	struct flame_desktop desktop;
	struct flame_input input;
	struct flame_idle idle;
	struct flame_xdg xdg;
	struct flame_layers layers;
	struct flame_lock lock;
	struct flame_ipc ipc;
	struct flame_xbridge xbridge;
	struct flame_xshell xshell;
	struct flame_capture capture;
	struct flame_picker picker;
	struct flame_screencast screencast;
	struct flame_eyecandy eyecandy;
	struct flame_wallpaper wallpaper;
	struct flame_portal portal;
	struct flame_xselection xselection;
	struct flame_ext_workspaces ext_workspaces;
	struct flame_greeter greeter;
};

bool server_init(struct flame_server *server, const struct flame_config *config);
// greeter_cmd: flame's the login screen, see greeter/greeter.h. NULL else
bool server_start(struct flame_server *server, const char *startup_cmd,
	const char *greeter_cmd);
void server_run(struct flame_server *server);
void server_terminate(struct flame_server *server);
void server_finish(struct flame_server *server);
// init.lua again, soon: not from inside the lua that asked for it
void server_reload_config(struct flame_server *server);

#endif
