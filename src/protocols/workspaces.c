#include "protocols/workspaces.h"

#include <stdint.h>
#include <stdio.h>
#include <wlr/types/wlr_ext_workspace_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/log.h>

#include "core/server.h"
#include "util/listener.h"

static void handle_output_destroy(struct wl_listener *listener, void *data);

static void leave_output(struct flame_ext_workspaces *workspaces) {
	if (!workspaces->output) {
		return;
	}
	wlr_ext_workspace_group_handle_v1_output_leave(workspaces->group, workspaces->output);
	listener_disconnect(&workspaces->output_destroy);
	workspaces->output = NULL;
}

static void sync(struct flame_ext_workspaces *workspaces) {
	struct flame_desktop *desktop = &workspaces->server->desktop;

	struct flame_output *primary = outputs_primary(&workspaces->server->outputs);
	struct wlr_output *output = primary ? primary->wlr : NULL;
	if (output != workspaces->output) {
		leave_output(workspaces);
		if (output) {
			wlr_ext_workspace_group_handle_v1_output_enter(workspaces->group, output);
			listener_connect(&workspaces->output_destroy, &output->events.destroy,
				handle_output_destroy);
			workspaces->output = output;
		}
	}

	for (int i = 0; i < FLAME_WORKSPACE_COUNT; i++) {
		struct flame_workspace *workspace = &desktop->workspaces[i];
		bool active = workspace == desktop->active;
		// wlroots only tells clients about what actually changed
		wlr_ext_workspace_handle_v1_set_active(workspaces->handles[i], active);
		wlr_ext_workspace_handle_v1_set_hidden(workspaces->handles[i],
			!active && wl_list_empty(&workspace->views));
	}
}

static void handle_desktop_changed(struct wl_listener *listener, void *data) {
	struct flame_ext_workspaces *workspaces =
		wl_container_of(listener, workspaces, desktop_changed);
	sync(workspaces);
}

// a monitor showed up or went away, the group follows the primary one
static void handle_layout_change(struct wl_listener *listener, void *data) {
	struct flame_ext_workspaces *workspaces =
		wl_container_of(listener, workspaces, layout_change);
	sync(workspaces);
}

static void handle_output_destroy(struct wl_listener *listener, void *data) {
	struct flame_ext_workspaces *workspaces =
		wl_container_of(listener, workspaces, output_destroy);
	leave_output(workspaces);
	// the output list may still have it, pick the next one once it's gone
	desktop_notify(&workspaces->server->desktop);
}

// a bar clicked a workspace. everything but activate is refused: they're
// fixed, numbered, and there's one group
static void handle_commit(struct wl_listener *listener, void *data) {
	struct flame_ext_workspaces *workspaces = wl_container_of(listener, workspaces, commit);
	struct wlr_ext_workspace_v1_commit_event *event = data;
	struct wlr_ext_workspace_v1_request *request;
	wl_list_for_each(request, event->requests, link) {
		if (request->type == WLR_EXT_WORKSPACE_V1_REQUEST_ACTIVATE
				&& request->activate.workspace) {
			int index = (int)(intptr_t)request->activate.workspace->data;
			desktop_switch_workspace(&workspaces->server->desktop, index);
		}
	}
}

bool ext_workspaces_init(struct flame_ext_workspaces *workspaces, struct flame_server *server) {
	workspaces->server = server;
	workspaces->manager = wlr_ext_workspace_manager_v1_create(server->display, 1);
	if (!workspaces->manager) {
		wlr_log(WLR_ERROR, "failed to create the ext-workspace manager");
		return false;
	}
	workspaces->group = wlr_ext_workspace_group_handle_v1_create(workspaces->manager, 0);
	if (!workspaces->group) {
		wlr_log(WLR_ERROR, "failed to create the ext-workspace group");
		return false;
	}
	for (int i = 0; i < FLAME_WORKSPACE_COUNT; i++) {
		// same ids and names as flame's ipc and the keys: 1..9, then 10
		char name[16];
		snprintf(name, sizeof(name), "%d", i + 1);
		struct wlr_ext_workspace_handle_v1 *handle = wlr_ext_workspace_handle_v1_create(
			workspaces->manager, name, EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE);
		if (!handle) {
			wlr_log(WLR_ERROR, "failed to create an ext-workspace handle");
			return false;
		}
		handle->data = (void *)(intptr_t)i;
		wlr_ext_workspace_handle_v1_set_name(handle, name);
		uint32_t coordinate = (uint32_t)i;
		wlr_ext_workspace_handle_v1_set_coordinates(handle, &coordinate, 1);
		wlr_ext_workspace_handle_v1_set_group(handle, workspaces->group);
		workspaces->handles[i] = handle;
	}

	wl_list_init(&workspaces->output_destroy.link);
	listener_connect(&workspaces->commit, &workspaces->manager->events.commit, handle_commit);
	listener_connect(&workspaces->desktop_changed, &server->desktop.events.changed,
		handle_desktop_changed);
	listener_connect(&workspaces->layout_change, &server->outputs.layout->events.change,
		handle_layout_change);
	sync(workspaces);
	return true;
}

void ext_workspaces_finish(struct flame_ext_workspaces *workspaces) {
	listener_disconnect(&workspaces->commit);
	listener_disconnect(&workspaces->desktop_changed);
	listener_disconnect(&workspaces->layout_change);
	listener_disconnect(&workspaces->output_destroy);
	workspaces->output = NULL;
	// the manager, group and handles go with the display
}
