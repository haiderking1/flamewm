#include "layers/layers.h"

#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_scene.h>

#include "core/server.h"
#include "util/macros.h"

static void arrange_pass(struct flame_layers *layers, struct flame_output *output,
		bool exclusive, const struct wlr_box *full, struct wlr_box *usable) {
	static const enum zwlr_layer_shell_v1_layer order[] = {
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
		ZWLR_LAYER_SHELL_V1_LAYER_TOP,
		ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM,
		ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
	};

	for (size_t i = 0; i < LENGTH(order); i++) {
		struct flame_layer_surface *surface;
		wl_list_for_each(surface, &layers->surfaces, link) {
			struct wlr_layer_surface_v1 *wlr = surface->wlr;
			// unmapped ones get re-initialized on their next commit
			if (surface->output != output || surface->layer != order[i]
					|| !wlr->initialized
					|| (wlr->current.exclusive_zone > 0) != exclusive) {
				continue;
			}
			// positions it, sends the configure, and shrinks usable by its zone
			wlr_scene_layer_surface_v1_configure(surface->scene, full, usable);
			wlr_scene_node_set_position(&surface->popups->node,
				surface->scene->tree->node.x, surface->scene->tree->node.y);
		}
	}
}

bool layers_arrange(struct flame_layers *layers, struct flame_output *output) {
	struct wlr_box full = output->box;
	struct wlr_box usable = output->box;

	// whoever reserves an edge goes first, so the rest lay out inside what's left
	arrange_pass(layers, output, true, &full, &usable);
	arrange_pass(layers, output, false, &full, &usable);

	if (wlr_box_equal(&usable, &output->usable)) {
		return false;
	}
	output->usable = usable;
	return true;
}
