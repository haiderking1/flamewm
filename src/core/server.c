#include "core/server.h"

#include <stdlib.h>
#include <wlr/util/log.h>

#include "config/config.h"
#include "input/keyboard.h"
#include "protocols/globals.h"
#include "scene/scene.h"
#include "util/spawn.h"

bool server_init(struct flame_server *server, const struct flame_config *config) {
	server->config = config;
	anims_init(&server->anims, server);
	server->display = wl_display_create();
	if (!server->display) {
		wlr_log(WLR_ERROR, "failed to create wl_display");
		return false;
	}
	server->loop = wl_display_get_event_loop(server->display);
	// what flame has for an app that's busy, not reading, waits here. past
	// libwayland's default 4kB the app is cut off and dies (a browser stuck
	// for a few seconds with the mouse moving over it), 1MB rides that out
	wl_display_set_default_max_buffer_size(server->display, 1024 * 1024);

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
		&& xbridge_init(&server->xbridge, server)
		&& xshell_init(&server->xshell, server, &server->xbridge.xserver)
		&& xselection_init(&server->xselection, server, &server->xbridge.xserver)
		&& ext_workspaces_init(&server->ext_workspaces, server)
		&& capture_init(&server->capture, server)
		&& picker_init(&server->picker, server)
		&& screencast_init(&server->screencast, server)
		&& portal_init(&server->portal, server)
		&& eyecandy_init(&server->eyecandy, server);
}

static void server_keymap_names(struct flame_server *server);

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
	// flame's own X server, $DISPLAY for everyone
	xbridge_start(&server->xbridge);
	// apps started by the session (launchers through systemd, dbus
	// activation) get the same, like sway's systemd integration. only on a
	// real seat: a nested or headless flame mustn't take over the session's
	if (server->backend.session) {
		const char *const argv[] = {"dbus-update-activation-environment", "--systemd",
			"WAYLAND_DISPLAY", "DISPLAY", "XDG_CURRENT_DESKTOP", "XDG_SESSION_TYPE",
			"FLAMEWM_SOCKET", NULL};
		spawn(argv);
	}

	wlr_log(WLR_INFO, "flamewm running on WAYLAND_DISPLAY=%s", server->socket);
	server_keymap_names(server);
	for (size_t i = 0; i < server->config->autostart_count; i++) {
		spawn_shell(server->config->autostart[i]);
	}
	if (startup_cmd) {
		spawn_shell(startup_cmd);
	}
	return true;
}

// X apps read the keymap's names off the root window
static void server_keymap_names(struct flame_server *server) {
	const struct flame_keyboard_names *k = &server->config->keyboard;
	const char *env_layout = getenv("XKB_DEFAULT_LAYOUT");
	xserver_set_keymap_names(&server->xbridge.xserver,
		k->rules ? k->rules : "evdev", k->model ? k->model : "pc105",
		k->layout ? k->layout : env_layout ? env_layout : "us",
		k->variant ? k->variant : "", k->options ? k->options : "");
}

// what changed takes effect: borders and colors on every window, gaps and
// border widths through a new layout, keymaps and repeat on every keyboard
static void server_apply_config(struct flame_server *server) {
	struct flame_desktop *desktop = &server->desktop;
	for (int i = 0; i < FLAME_WORKSPACE_COUNT; i++) {
		struct flame_workspace *workspace = &desktop->workspaces[i];
		struct flame_view *view;
		wl_list_for_each(view, &workspace->views, link) {
			view_refresh_border(view, view == desktop->focused);
		}
		wl_list_for_each(view, &workspace->floats, link) {
			view_refresh_border(view, view == desktop->focused);
		}
	}
	struct flame_output *output;
	wl_list_for_each(output, &server->outputs.list, link) {
		scene_rect_set_color(output->background, server->config->background_color);
	}
	keyboards_apply_config(&server->input);
	server_keymap_names(server);
	outputs_apply_config(&server->outputs);
	xbridge_config_changed(&server->xbridge);
	desktop_arrange(desktop);
}

static void handle_reload_idle(void *data) {
	struct flame_server *server = data;
	server->reload_idle = NULL;
	char *error = NULL;
	struct flame_config *config = config_load(server, server->config_path, &error);
	if (!config) {
		wlr_log(WLR_ERROR, "config: no memory to reload");
		return;
	}
	if (error) {
		wlr_log(WLR_ERROR, "config: %s", error);
		free(error);
	} else {
		wlr_log(WLR_INFO, "config: reloaded %s", server->config_path ? server->config_path
			: config_default_path());
	}
	struct flame_config *old = (struct flame_config *)server->config;
	server->config = config;
	server_apply_config(server);
	config_free(old);
}

void server_reload_config(struct flame_server *server) {
	if (!server->reload_idle) {
		server->reload_idle = wl_event_loop_add_idle(server->loop, handle_reload_idle, server);
	}
}

void server_run(struct flame_server *server) {
	wl_display_run(server->display);
}

void server_terminate(struct flame_server *server) {
	wl_display_terminate(server->display);
}

void server_finish(struct flame_server *server) {
	if (server->reload_idle) {
		wl_event_source_remove(server->reload_idle);
		server->reload_idle = NULL;
	}
	server->terminating = true;
	wl_display_destroy_clients(server->display);
	anims_finish(&server->anims);
	// stop listening to the desktop before it goes
	ipc_finish(&server->ipc);
	xselection_finish(&server->xselection);
	xbridge_finish(&server->xbridge);
	xshell_finish(&server->xshell);
	eyecandy_finish(&server->eyecandy);
	portal_finish(&server->portal);
	screencast_finish(&server->screencast);
	picker_finish(&server->picker);
	capture_finish(&server->capture);
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
