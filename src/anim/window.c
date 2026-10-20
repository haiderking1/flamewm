#include "anim/window.h"

#include <stdlib.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/util/log.h>

#include "anim/ghost.h"
#include "config/config.h"
#include "core/server.h"
#include "render/snapshot.h"
#include "scene/scene.h"
#include "util/listener.h"

enum kind {
	KIND_NONE,
	KIND_OPEN,
	KIND_MOVE,
};

struct flame_view_anim {
	struct flame_view *view;
	struct flame_anim anim; // opening or moving
	enum kind kind;
	// open: how it comes in
	enum flame_anim_type type;
	bool fade;
	double slide_dx, slide_dy;
	// move: drawn at from, going to to
	struct wlr_box from, to;
	struct flame_effect shown; // what the last step drew, a new move starts there
	// focus: faded to this, fading towards focus_to
	struct flame_anim focus_anim;
	float focus_opacity, focus_from, focus_to;
	// a snapshot taken ahead of time, for the close
	struct wlr_texture *snapshot;
	struct wlr_box snapshot_box;
};

static const struct flame_anim_config *config_of(struct flame_view *view) {
	return &view->desktop->server->config->anim;
}

static struct flame_anims *anims_of(struct flame_view *view) {
	return &view->desktop->server->anims;
}

static bool wanted(struct flame_view *view) {
	return config_of(view)->enabled && !view->rule.no_animation;
}

static struct flame_view_anim *state_of(struct flame_view *view) {
	if (!view->anim) {
		view->anim = calloc(1, sizeof(*view->anim));
		if (view->anim) {
			view->anim->view = view;
			view->anim->focus_opacity = 1;
			view->anim->shown = (struct flame_effect){1, 1, 1, 0, 0, 0, 0};
		}
	}
	return view->anim;
}

static struct flame_effect identity(void) {
	return (struct flame_effect){1, 1, 1, 0, 0, 0, 0};
}

static bool is_identity(const struct flame_effect *e) {
	return e->opacity >= 1 && e->scale_x == 1 && e->scale_y == 1 && e->dx == 0 && e->dy == 0;
}

// the tween's effect with focus fading on top
static void apply(struct flame_view_anim *state, struct flame_effect effect) {
	state->shown = effect;
	effect.opacity *= state->focus_opacity;
	scene_tree_set_effect(state->view->tree, is_identity(&effect) ? NULL : &effect);
}

// where the nearest screen edge is: dx, dy that take box just past it
static void slide_offset(struct flame_view *view, struct wlr_box box, double *dx,
		double *dy) {
	struct flame_output *output = outputs_primary(&view->desktop->server->outputs);
	struct wlr_box area = output ? output->box : box;
	double left = box.x + box.width - area.x;
	double right = area.x + area.width - box.x;
	double top = box.y + box.height - area.y;
	double bottom = area.y + area.height - box.y;
	double nearest = left;
	*dx = -left;
	*dy = 0;
	if (right < nearest) {
		nearest = right;
		*dx = right;
	}
	if (top < nearest) {
		nearest = top;
		*dx = 0;
		*dy = -top;
	}
	if (bottom < nearest) {
		*dx = 0;
		*dy = bottom;
	}
}

static enum flame_anim_type type_of(int rule, enum flame_anim_type fallback) {
	return rule >= 0 ? (enum flame_anim_type)rule : fallback;
}

// ---- opening and moving ----

static void step(struct flame_anim *anim, double eased, double t) {
	struct flame_view_anim *state = anim->data;
	struct flame_view *view = state->view;
	const struct flame_anim_config *config = config_of(view);
	struct flame_effect e = identity();
	if (state->kind == KIND_OPEN) {
		int w = view->geometry.width, h = view->geometry.height;
		if (state->type == FLAME_ANIM_SLIDE) {
			e.dx = lerp(state->slide_dx, 0, eased);
			e.dy = lerp(state->slide_dy, 0, eased);
		} else if (state->type == FLAME_ANIM_ZOOM) {
			double s = lerp(config->zoom_from, 1, eased);
			e.scale_x = s;
			e.scale_y = s;
			e.origin_x = w / 2.0;
			e.origin_y = h / 2.0;
		}
		if (state->type == FLAME_ANIM_FADE) {
			e.opacity = (float)curve_ease(&config->fade_in_curve, t);
		} else if (state->fade) {
			e.opacity = (float)lerp(config->fade_in_from, 1,
				curve_ease(&config->fade_in_curve, t));
		}
	} else if (state->kind == KIND_MOVE) {
		// the tree sits at to: scaled and moved so it shows at the box between
		double x = lerp(state->from.x, state->to.x, eased);
		double y = lerp(state->from.y, state->to.y, eased);
		double w = lerp(state->from.width, state->to.width, eased);
		double h = lerp(state->from.height, state->to.height, eased);
		e.scale_x = state->to.width > 0 ? w / state->to.width : 1;
		e.scale_y = state->to.height > 0 ? h / state->to.height : 1;
		e.dx = x - state->to.x;
		e.dy = y - state->to.y;
	}
	apply(state, e);
}

static void done(struct flame_anim *anim, bool finished) {
	struct flame_view_anim *state = anim->data;
	state->kind = KIND_NONE;
	apply(state, identity());
}

// where the window shows right now, layout coords: a move picks up from there
static struct wlr_box shown_box(struct flame_view_anim *state, struct wlr_box placed) {
	const struct flame_effect *e = &state->shown;
	return (struct wlr_box){
		.x = placed.x + (int)(e->dx + e->origin_x * (1 - e->scale_x)),
		.y = placed.y + (int)(e->dy + e->origin_y * (1 - e->scale_y)),
		.width = (int)(placed.width * e->scale_x),
		.height = (int)(placed.height * e->scale_y),
	};
}

void window_anim_placed(struct flame_view *view, struct wlr_box was, bool first) {
	bool by_hand = view->placed_by_hand;
	view->placed_by_hand = false;
	if (by_hand || !wanted(view) || view->grabbed || view->resizing
			|| view->workspace != view->desktop->active) {
		window_anim_stop(view);
		return;
	}
	const struct flame_anim_config *config = config_of(view);
	struct flame_view_anim *state = state_of(view);
	if (!state) {
		return;
	}
	state->anim.step = step;
	state->anim.done = done;
	state->anim.data = state;
	if (first) {
		state->type = type_of(view->rule.open, config->open);
		if (state->type == FLAME_ANIM_NONE) {
			return;
		}
		state->kind = KIND_OPEN;
		state->fade = config->fade_in && !view->rule.no_fade_in;
		slide_offset(view, view->geometry, &state->slide_dx, &state->slide_dy);
		anim_start(anims_of(view), &state->anim, config->open_ms, &config->open_curve);
		return;
	}
	if (wlr_box_equal(&was, &view->geometry) || wlr_box_empty(&was)) {
		return;
	}
	// from wherever it shows now, a move cut short by another included
	struct wlr_box from = state->kind == KIND_MOVE && anim_running(&state->anim)
		? shown_box(state, state->to) : was;
	if (state->kind == KIND_OPEN && anim_running(&state->anim)) {
		anim_finish(&state->anim);
	}
	state->kind = KIND_MOVE;
	state->from = from;
	state->to = view->geometry;
	anim_start(anims_of(view), &state->anim, config->move_ms, &config->move_curve);
}

void window_anim_stop(struct flame_view *view) {
	if (view->anim && anim_running(&view->anim->anim)) {
		anim_finish(&view->anim->anim);
	}
}

// ---- focus ----

static void focus_step(struct flame_anim *anim, double eased, double t) {
	struct flame_view_anim *state = anim->data;
	state->focus_opacity = (float)lerp(state->focus_from, state->focus_to, eased);
	apply(state, state->shown);
}

void window_anim_focus(struct flame_view *view, bool focused) {
	const struct flame_anim_config *config = config_of(view);
	float target = view->rule.opacity > 0 ? view->rule.opacity
		: focused ? config->focused_opacity : config->unfocused_opacity;
	struct flame_view_anim *state = view->anim;
	if (!state && target >= 1) {
		return; // never faded, nothing to do
	}
	state = state_of(view);
	if (!state || state->focus_to == target) {
		return;
	}
	state->focus_from = state->focus_opacity;
	state->focus_to = target;
	state->focus_anim.step = focus_step;
	state->focus_anim.data = state;
	anim_start(anims_of(view), &state->focus_anim, wanted(view) ? config->focus_ms : 0,
		&config->focus_curve);
}

// ---- closing ----

void window_anim_snapshot(struct flame_view *view) {
	if (!wanted(view) || !view->shown || view->desktop->server->terminating || view->workspace != view->desktop->active
			|| type_of(view->rule.close, config_of(view)->close) == FLAME_ANIM_NONE) {
		return;
	}
	struct flame_view_anim *state = state_of(view);
	if (!state) {
		return;
	}
	if (state->snapshot) {
		wlr_texture_destroy(state->snapshot);
	}
	// drawn plain: no tween, no focus fade
	struct flame_effect was = state->shown;
	state->snapshot_box = view->geometry;
	struct flame_output *output = outputs_primary(&view->desktop->server->outputs);
	state->snapshot = render_snapshot(&view->desktop->server->render,
		output ? output->wlr : NULL, &view->tree->node, view->geometry);
	state->shown = was;
}

void window_anim_snapshot_if_missing(struct flame_view *view) {
	if (!view->anim || !view->anim->snapshot) {
		window_anim_snapshot(view);
	}
}

void window_anim_closed(struct flame_view *view) {
	struct flame_view_anim *state = view->anim;
	if (!state || !state->snapshot) {
		return;
	}
	const struct flame_anim_config *config = config_of(view);
	struct wlr_texture *texture = state->snapshot;
	struct wlr_box box = state->snapshot_box;
	state->snapshot = NULL;
	if (state->anim.running) {
		anim_cancel(&state->anim);
	}
	struct flame_tree *parent = view->tree->node.parent;
	if (!parent) {
		wlr_texture_destroy(texture);
		return;
	}
	struct ghost_style style = {
		.type = type_of(view->rule.close, config->close),
		.fade = config->fade_out && !view->rule.no_fade_out,
		.fade_from = config->fade_out_from,
		.zoom_to = config->zoom_to,
		.ms = config->close_ms,
		.curve = config->close_curve,
	};
	slide_offset(view, box, &style.slide_dx, &style.slide_dy);
	ghost_play(anims_of(view), parent, texture, box, &style);
}

void window_anim_discard(struct flame_view *view) {
	if (view->anim && view->anim->snapshot) {
		wlr_texture_destroy(view->anim->snapshot);
		view->anim->snapshot = NULL;
	}
}

void window_anim_free(struct flame_view *view) {
	struct flame_view_anim *state = view->anim;
	if (!state) {
		return;
	}
	anim_cancel(&state->anim);
	anim_cancel(&state->focus_anim);
	if (state->snapshot) {
		wlr_texture_destroy(state->snapshot);
	}
	free(state);
	view->anim = NULL;
}
