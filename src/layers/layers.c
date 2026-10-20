#include "layers/layers.h"

#include <stdlib.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "core/server.h"
#include "shell/popup.h"
#include "util/listener.h"
#include "util/macros.h"

static struct wlr_scene_tree *scene_layer(struct flame_server *server,
		enum zwlr_layer_shell_v1_layer layer) {
	switch (layer) {
	case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
		return server->render.layers[FLAME_LAYER_BACKGROUND];
	case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
		return server->render.layers[FLAME_LAYER_BOTTOM];
	case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
		return server->render.layers[FLAME_LAYER_TOP];
	case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
		break;
	}
	return server->render.layers[FLAME_LAYER_OVERLAY];
}

// a bottom layer's menus would end up under the tiles otherwise
static struct wlr_scene_tree *popup_layer(struct flame_server *server,
		enum zwlr_layer_shell_v1_layer layer) {
	return scene_layer(server, MAX(layer, ZWLR_LAYER_SHELL_V1_LAYER_TOP));
}

struct flame_layer_surface *layer_surface_from_wlr(struct wlr_surface *surface) {
	struct wlr_layer_surface_v1 *wlr =
		wlr_layer_surface_v1_try_from_wlr_surface(wlr_surface_get_root_surface(surface));
	return wlr ? wlr->data : NULL;
}

static void rearrange(struct flame_layer_surface *surface) {
	if (surface->output && layers_arrange(surface->layers, surface->output)) {
		desktop_arrange(&surface->layers->server->desktop);
	}
}

static void handle_map(struct wl_listener *listener, void *data) {
	struct flame_layer_surface *surface = wl_container_of(listener, surface, map);
	wlr_log(WLR_DEBUG, "map layer surface namespace=%s", surface->wlr->namespace);
	// exclusive zones only count once mapped
	rearrange(surface);
	layers_update_focus(surface->layers);
}

static void handle_unmap(struct wl_listener *listener, void *data) {
	struct flame_layer_surface *surface = wl_container_of(listener, surface, unmap);
	rearrange(surface);
	layers_update_focus(surface->layers);
}

static void handle_commit(struct wl_listener *listener, void *data) {
	struct flame_layer_surface *surface = wl_container_of(listener, surface, commit);
	struct flame_server *server = surface->layers->server;
	struct wlr_layer_surface_v1 *wlr = surface->wlr;

	if (!surface->output) {
		return;
	}
	if (wlr->current.layer != surface->layer) {
		surface->layer = wlr->current.layer;
		wlr_scene_node_reparent(&surface->scene->tree->node,
			scene_layer(server, surface->layer));
		wlr_scene_node_reparent(&surface->popups->node, popup_layer(server, surface->layer));
	}
	// first commit needs its configure, later ones only when state changed,
	// not every frame
	if (wlr->initial_commit || wlr->current.committed) {
		rearrange(surface);
		layers_update_focus(surface->layers);
	}
}

static void handle_new_popup(struct wl_listener *listener, void *data) {
	struct flame_layer_surface *surface = wl_container_of(listener, surface, new_popup);
	popup_create(surface->layers->server, data, surface->popups);
}

static void handle_destroy(struct wl_listener *listener, void *data) {
	struct flame_layer_surface *surface = wl_container_of(listener, surface, destroy);
	struct flame_layers *layers = surface->layers;

	listener_disconnect(&surface->map);
	listener_disconnect(&surface->unmap);
	listener_disconnect(&surface->commit);
	listener_disconnect(&surface->new_popup);
	listener_disconnect(&surface->destroy);

	wl_list_remove(&surface->link);
	// the scene helper drops its own tree, the popup tree is ours
	wlr_scene_node_destroy(&surface->popups->node);
	surface->wlr->data = NULL;

	struct flame_output *output = surface->output;
	bool had_keyboard = layers->focused == surface;
	if (had_keyboard) {
		layers->focused = NULL;
	}
	free(surface);

	if (output && layers_arrange(layers, output)) {
		desktop_arrange(&layers->server->desktop);
	}
	if (had_keyboard) {
		focus_restore_keyboard(&layers->server->desktop);
	}
	layers_update_focus(layers);
}

static void handle_new_surface(struct wl_listener *listener, void *data) {
	struct flame_layers *layers = wl_container_of(listener, layers, new_surface);
	struct flame_server *server = layers->server;
	struct wlr_layer_surface_v1 *wlr = data;

	// no output asked for: the one tiles live on
	if (!wlr->output) {
		struct flame_output *primary = outputs_primary(&server->outputs);
		if (!primary) {
			wlr_log(WLR_INFO, "layer surface with no output to go on, closing it");
			wlr_layer_surface_v1_destroy(wlr);
			return;
		}
		wlr->output = primary->wlr;
	}

	struct flame_layer_surface *surface = calloc(1, sizeof(*surface));
	if (!surface) {
		wlr_log(WLR_ERROR, "out of memory");
		wlr_layer_surface_v1_destroy(wlr);
		return;
	}
	surface->layers = layers;
	surface->wlr = wlr;
	surface->output = wlr->output->data;
	surface->layer = wlr->pending.layer;
	surface->scene = wlr_scene_layer_surface_v1_create(scene_layer(server, surface->layer), wlr);
	surface->popups = wlr_scene_tree_create(popup_layer(server, surface->layer));
	if (!surface->scene || !surface->popups) {
		wlr_log(WLR_ERROR, "failed to create layer surface scene");
		if (surface->popups) {
			wlr_scene_node_destroy(&surface->popups->node);
		}
		free(surface);
		wlr_layer_surface_v1_destroy(wlr);
		return;
	}
	wlr->data = surface;

	listener_connect(&surface->map, &wlr->surface->events.map, handle_map);
	listener_connect(&surface->unmap, &wlr->surface->events.unmap, handle_unmap);
	listener_connect(&surface->commit, &wlr->surface->events.commit, handle_commit);
	listener_connect(&surface->new_popup, &wlr->events.new_popup, handle_new_popup);
	listener_connect(&surface->destroy, &wlr->events.destroy, handle_destroy);
	wl_list_insert(layers->surfaces.prev, &surface->link);
}

bool layers_init(struct flame_layers *layers, struct flame_server *server) {
	layers->server = server;
	wl_list_init(&layers->surfaces);
	layers->focused = NULL;

	layers->shell = wlr_layer_shell_v1_create(server->display, 5);
	if (!layers->shell) {
		wlr_log(WLR_ERROR, "failed to create layer-shell");
		return false;
	}
	listener_connect(&layers->new_surface, &layers->shell->events.new_surface,
		handle_new_surface);
	return true;
}

void layers_finish(struct flame_layers *layers) {
	listener_disconnect(&layers->new_surface);
}

void layers_output_gone(struct flame_layers *layers, struct flame_output *output) {
	struct flame_layer_surface *surface, *tmp;
	wl_list_for_each_safe(surface, tmp, &layers->surfaces, link) {
		if (surface->output == output) {
			// sends closed, the client's destroy lands in handle_destroy
			surface->output = NULL;
			wlr_layer_surface_v1_destroy(surface->wlr);
		}
	}
}
