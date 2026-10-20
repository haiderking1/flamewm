#include "anim/layer.h"

#include <stdlib.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_layer_shell_v1.h>

#include "anim/ghost.h"
#include "config/config.h"
#include "core/server.h"
#include "render/snapshot.h"
#include "scene/scene.h"

struct flame_layer_anim {
	struct flame_layer_surface *surface;
	struct flame_anim anim;
	enum flame_anim_type type;
	bool fade;
	double dx, dy; // where a slide starts
	int width, height;
	struct wlr_texture *snapshot;
	struct wlr_box snapshot_box;
};

static struct flame_server *server_of(struct flame_layer_surface *surface) {
	return surface->layers->server;
}

static const struct flame_anim_config *config_of(struct flame_layer_surface *surface) {
	return &server_of(surface)->config->anim;
}

static struct wlr_box box_of(struct flame_layer_surface *surface) {
	struct wlr_surface *wlr = surface->wlr->surface;
	return (struct wlr_box){surface->tree->node.x, surface->tree->node.y,
		wlr->current.width, wlr->current.height};
}

// off the edge it's anchored to: a top bar comes down from the top. one
// anchored to nothing, or everything, takes the nearest edge
static void slide_from(struct flame_layer_surface *surface, struct wlr_box box, double *dx,
		double *dy) {
	struct wlr_box area = surface->output ? surface->output->box : box;
	uint32_t anchor = surface->wlr->current.anchor;
	bool top = anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
	bool bottom = anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
	bool left = anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
	bool right = anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	double to_top = box.y + box.height - area.y, to_bottom = area.y + area.height - box.y;
	double to_left = box.x + box.width - area.x, to_right = area.x + area.width - box.x;
	*dx = 0;
	*dy = 0;
	if (top != bottom) {
		*dy = top ? -to_top : to_bottom;
	} else if (left != right) {
		*dx = left ? -to_left : to_right;
	} else {
		double nearest = to_top;
		*dy = -to_top;
		if (to_bottom < nearest) {
			nearest = to_bottom;
			*dy = to_bottom;
		}
		if (to_left < nearest) {
			nearest = to_left;
			*dy = 0;
			*dx = -to_left;
		}
		if (to_right < nearest) {
			*dy = 0;
			*dx = to_right;
		}
	}
}

static struct flame_layer_anim *state_of(struct flame_layer_surface *surface) {
	if (!surface->anim) {
		surface->anim = calloc(1, sizeof(*surface->anim));
		if (surface->anim) {
			surface->anim->surface = surface;
		}
	}
	return surface->anim;
}

static void step(struct flame_anim *anim, double eased, double t) {
	struct flame_layer_anim *state = anim->data;
	const struct flame_anim_config *config = config_of(state->surface);
	struct flame_effect e = {1, 1, 1, 0, 0, 0, 0};
	if (state->type == FLAME_ANIM_SLIDE) {
		e.dx = lerp(state->dx, 0, eased);
		e.dy = lerp(state->dy, 0, eased);
	} else if (state->type == FLAME_ANIM_ZOOM) {
		double s = lerp(config->zoom_from, 1, eased);
		e.scale_x = s;
		e.scale_y = s;
		e.origin_x = state->width / 2.0;
		e.origin_y = state->height / 2.0;
	}
	if (state->type == FLAME_ANIM_FADE) {
		e.opacity = (float)curve_ease(&config->fade_in_curve, t);
	} else if (state->fade) {
		e.opacity = (float)lerp(config->fade_in_from, 1, curve_ease(&config->fade_in_curve, t));
	}
	scene_tree_set_effect(state->surface->tree, &e);
}

static void done(struct flame_anim *anim, bool finished) {
	struct flame_layer_anim *state = anim->data;
	scene_tree_set_effect(state->surface->tree, NULL);
}

void layer_anim_mapped(struct flame_layer_surface *surface) {
	const struct flame_anim_config *config = config_of(surface);
	if (!config->layers || config->layer_open == FLAME_ANIM_NONE) {
		return;
	}
	struct flame_layer_anim *state = state_of(surface);
	if (!state) {
		return;
	}
	struct wlr_box box = box_of(surface);
	state->type = config->layer_open;
	state->fade = config->fade_in;
	state->width = box.width;
	state->height = box.height;
	slide_from(surface, box, &state->dx, &state->dy);
	state->anim.step = step;
	state->anim.done = done;
	state->anim.data = state;
	anim_start(&server_of(surface)->anims, &state->anim, config->open_ms, &config->open_curve);
}

void layer_anim_snapshot(struct flame_layer_surface *surface) {
	const struct flame_anim_config *config = config_of(surface);
	if (!config->layers || config->layer_close == FLAME_ANIM_NONE || !surface->output) {
		return;
	}
	struct flame_layer_anim *state = state_of(surface);
	if (!state) {
		return;
	}
	if (state->snapshot) {
		wlr_texture_destroy(state->snapshot);
	}
	state->snapshot_box = box_of(surface);
	state->snapshot = render_snapshot(&server_of(surface)->render, surface->output->wlr,
		&surface->tree->node, state->snapshot_box);
}

void layer_anim_unmapped(struct flame_layer_surface *surface) {
	struct flame_layer_anim *state = surface->anim;
	if (!state || !state->snapshot) {
		layer_anim_snapshot(surface);
		state = surface->anim;
	}
	if (!state || !state->snapshot) {
		return;
	}
	if (anim_running(&state->anim)) {
		anim_cancel(&state->anim);
	}
	const struct flame_anim_config *config = config_of(surface);
	struct wlr_texture *texture = state->snapshot;
	struct wlr_box box = state->snapshot_box;
	state->snapshot = NULL;
	struct flame_tree *parent = surface->tree->node.parent;
	if (!parent) {
		wlr_texture_destroy(texture);
		return;
	}
	struct ghost_style style = {
		.type = config->layer_close,
		.fade = config->fade_out,
		.fade_from = config->fade_out_from,
		.zoom_to = config->zoom_to,
		.ms = config->close_ms,
		.curve = config->close_curve,
	};
	slide_from(surface, box, &style.slide_dx, &style.slide_dy);
	ghost_play(&server_of(surface)->anims, parent, texture, box, &style);
}

void layer_anim_free(struct flame_layer_surface *surface) {
	struct flame_layer_anim *state = surface->anim;
	if (!state) {
		return;
	}
	anim_cancel(&state->anim);
	if (state->snapshot) {
		wlr_texture_destroy(state->snapshot);
	}
	free(state);
	surface->anim = NULL;
}
