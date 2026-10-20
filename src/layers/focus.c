#include "layers/layers.h"

#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/util/log.h>

#include "core/server.h"

static bool wants_keyboard(struct flame_layer_surface *surface) {
	return surface->wlr->surface->mapped && surface->wlr->current.keyboard_interactive
		!= ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE;
}

static bool is_exclusive(struct flame_layer_surface *surface) {
	// the protocol only lets top and overlay grab the keyboard outright
	return surface->wlr->surface->mapped
		&& surface->wlr->current.keyboard_interactive
			== ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE
		&& surface->layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP;
}

// topmost layer that grabs the keyboard, the newest one wins inside a layer
static struct flame_layer_surface *exclusive_surface(struct flame_layers *layers) {
	struct flame_layer_surface *best = NULL;
	struct flame_layer_surface *surface;
	wl_list_for_each(surface, &layers->surfaces, link) {
		if (is_exclusive(surface) && (!best || surface->layer >= best->layer)) {
			best = surface;
		}
	}
	return best;
}

static void give(struct flame_layers *layers, struct flame_layer_surface *surface) {
	wlr_log(WLR_DEBUG, "layer %s takes the keyboard", surface->wlr->namespace);
	layers->focused = surface;
	input_keyboard_enter(&layers->server->input, surface->wlr->surface);
}

void layers_update_focus(struct flame_layers *layers) {
	struct flame_layer_surface *exclusive = exclusive_surface(layers);
	if (exclusive) {
		if (layers->focused != exclusive) {
			give(layers, exclusive);
		}
		return;
	}

	// whoever had it unmapped or stopped wanting it. destroyed ones already
	// cleared this themselves, so it never dangles
	if (layers->focused && !wants_keyboard(layers->focused)) {
		wlr_log(WLR_DEBUG, "layer %s gives the keyboard back", layers->focused->wlr->namespace);
		layers->focused = NULL;
		focus_restore_keyboard(&layers->server->desktop);
	}
}

bool layers_keyboard_locked(struct flame_layers *layers) {
	return layers->focused && is_exclusive(layers->focused);
}

void layers_drop_keyboard(struct flame_layers *layers) {
	layers->focused = NULL;
}

void layers_click(struct flame_layers *layers, struct wlr_surface *surface) {
	struct flame_layer_surface *layer = layer_surface_from_wlr(surface);
	if (layer && wants_keyboard(layer) && !layers_keyboard_locked(layers)) {
		give(layers, layer);
	}
}
