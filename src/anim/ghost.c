#include "anim/ghost.h"

#include <stdlib.h>
#include <wlr/render/wlr_texture.h>

#include "anim/anim.h"
#include "scene/scene.h"
#include "util/listener.h"

struct ghost {
	struct flame_tree *tree;
	struct flame_buffer_node *buffer;
	struct wlr_texture *texture;
	struct flame_anim anim;
	struct ghost_style style;
	int width, height;
	struct wl_listener tree_destroy;
};

static void ghost_free(struct ghost *ghost) {
	listener_disconnect(&ghost->tree_destroy);
	if (ghost->tree) {
		scene_node_destroy(&ghost->tree->node);
	}
	wlr_texture_destroy(ghost->texture);
	free(ghost);
}

static void step(struct flame_anim *anim, double eased, double t) {
	struct ghost *ghost = anim->data;
	const struct ghost_style *style = &ghost->style;
	struct flame_effect e = {1, 1, 1, 0, 0, 0, 0};
	if (style->type == FLAME_ANIM_SLIDE) {
		e.dx = lerp(0, style->slide_dx, eased);
		e.dy = lerp(0, style->slide_dy, eased);
	} else if (style->type == FLAME_ANIM_ZOOM) {
		double s = lerp(1, style->zoom_to, eased);
		e.scale_x = s;
		e.scale_y = s;
		e.origin_x = ghost->width / 2.0;
		e.origin_y = ghost->height / 2.0;
	}
	if (style->type == FLAME_ANIM_FADE) {
		e.opacity = (float)(1 - t);
	} else if (style->fade) {
		e.opacity = (float)lerp(1, style->fade_from, t);
	}
	scene_tree_set_effect(ghost->tree, &e);
}

static void done(struct flame_anim *anim, bool finished) {
	ghost_free(anim->data);
}

static void handle_tree_destroy(struct wl_listener *listener, void *data) {
	// what it lived in went (flame shutting down): the animation goes too
	struct ghost *ghost = wl_container_of(listener, ghost, tree_destroy);
	listener_disconnect(&ghost->tree_destroy);
	ghost->tree = NULL;
	anim_cancel(&ghost->anim);
}

void ghost_play(struct flame_anims *anims, struct flame_tree *parent,
		struct wlr_texture *texture, struct wlr_fbox src, struct wlr_box box,
		const struct ghost_style *style) {
	struct ghost *ghost = calloc(1, sizeof(*ghost));
	if (!ghost || style->type == FLAME_ANIM_NONE) {
		wlr_texture_destroy(texture);
		free(ghost);
		return;
	}
	wl_list_init(&ghost->tree_destroy.link);
	ghost->texture = texture;
	ghost->style = *style;
	ghost->width = box.width;
	ghost->height = box.height;
	ghost->tree = scene_tree_create(parent);
	ghost->buffer = ghost->tree ? scene_buffer_create(ghost->tree, NULL, NULL) : NULL;
	if (!ghost->buffer) {
		ghost_free(ghost);
		return;
	}
	scene_node_set_position(&ghost->tree->node, box.x, box.y);
	scene_buffer_set(ghost->buffer, texture, texture, box.width, box.height, false, NULL, 0);
	scene_buffer_set_source(ghost->buffer, &src);
	listener_connect(&ghost->tree_destroy, &ghost->tree->node.events.destroy,
		handle_tree_destroy);
	ghost->anim.step = step;
	ghost->anim.done = done;
	ghost->anim.data = ghost;
	anim_start(anims, &ghost->anim, style->ms, &style->curve);
}
