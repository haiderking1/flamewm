#ifndef FLAME_PROTOCOLS_WORKSPACES_H
#define FLAME_PROTOCOLS_WORKSPACES_H

#include <stdbool.h>
#include <wayland-server-core.h>

#include "desktop/workspace.h"

struct flame_server;

// ext-workspace-v1, so waybar's ext/workspaces (and anything else speaking
// it) shows flame's workspaces and can switch them. just an adapter: the
// truth is the desktop, flame's own ipc says the same things. empty
// workspaces you're not on are marked hidden, bars hide those by default
struct flame_ext_workspaces {
	struct flame_server *server;
	struct wlr_ext_workspace_manager_v1 *manager;
	struct wlr_ext_workspace_group_handle_v1 *group; // single output for now
	struct wlr_ext_workspace_handle_v1 *handles[FLAME_WORKSPACE_COUNT];
	struct wlr_output *output; // the one the group is on, NULL for none

	struct wl_listener commit;
	struct wl_listener desktop_changed;
	struct wl_listener layout_change;
	struct wl_listener output_destroy;
};

bool ext_workspaces_init(struct flame_ext_workspaces *workspaces, struct flame_server *server);
void ext_workspaces_finish(struct flame_ext_workspaces *workspaces);

#endif
