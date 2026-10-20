#include "layers/layers.h"

#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/util/edges.h>

#include "core/server.h"
#include "util/macros.h"

// what an exclusive zone takes off the usable area, on the edge it's anchored to
static void reserve(const struct wlr_layer_surface_v1_state *state, enum wlr_edges edge,
		struct wlr_box *usable) {
	switch (edge) {
	case WLR_EDGE_NONE:
		return;
	case WLR_EDGE_TOP:
		usable->y += state->exclusive_zone + state->margin.top;
		usable->height -= state->exclusive_zone + state->margin.top;
		break;
	case WLR_EDGE_BOTTOM:
		usable->height -= state->exclusive_zone + state->margin.bottom;
		break;
	case WLR_EDGE_LEFT:
		usable->x += state->exclusive_zone + state->margin.left;
		usable->width -= state->exclusive_zone + state->margin.left;
		break;
	case WLR_EDGE_RIGHT:
		usable->width -= state->exclusive_zone + state->margin.right;
		break;
	}
	usable->width = MAX(usable->width, 0);
	usable->height = MAX(usable->height, 0);
}

// one axis: stretched between both anchors when it asked for size 0, else
// pinned to the anchored side, centered between two anchors or none
static void place_axis(int bounds_start, int bounds_size, int margin_start, int margin_end,
		bool anchor_start, bool anchor_end, int *start, int *size) {
	if (*size == 0) {
		*start = bounds_start + margin_start;
		*size = bounds_size - (margin_start + margin_end);
	} else if (anchor_start && !anchor_end) {
		*start = bounds_start + margin_start;
	} else if (anchor_end && !anchor_start) {
		*start = bounds_start + bounds_size - *size - margin_end;
	} else {
		*start = bounds_start + bounds_size / 2 - *size / 2;
	}
}

// same rules as every wlroots compositor, so bars land where they expect
static void configure(struct flame_layer_surface *surface, const struct wlr_box *full,
		struct wlr_box *usable) {
	struct wlr_layer_surface_v1 *wlr = surface->wlr;
	const struct wlr_layer_surface_v1_state *state = &wlr->current;

	// -1 means "ignore everyone else's zones", full output
	const struct wlr_box *bounds = state->exclusive_zone == -1 ? full : usable;
	struct wlr_box box = {
		.width = (int)state->desired_width,
		.height = (int)state->desired_height,
	};
	place_axis(bounds->x, bounds->width, state->margin.left, state->margin.right,
		state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT,
		state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT, &box.x, &box.width);
	place_axis(bounds->y, bounds->height, state->margin.top, state->margin.bottom,
		state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP,
		state->anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM, &box.y, &box.height);

	scene_node_set_position(&surface->tree->node, box.x, box.y);
	scene_node_set_position(&surface->popups->node, box.x, box.y);
	wlr_layer_surface_v1_configure(wlr, (uint32_t)MAX(box.width, 0), (uint32_t)MAX(box.height, 0));

	if (wlr->surface->mapped && state->exclusive_zone > 0) {
		reserve(state, wlr_layer_surface_v1_get_exclusive_edge(wlr), usable);
	}
}

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
			configure(surface, full, usable);
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
