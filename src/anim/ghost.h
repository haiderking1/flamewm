#ifndef FLAME_ANIM_GHOST_H
#define FLAME_ANIM_GHOST_H

#include <stdbool.h>
#include <wlr/util/box.h>

#include "config/config.h"

struct flame_anims;
struct flame_tree;
struct wlr_texture;

// something that closed, still on screen as its last picture while it plays
// its way out: slides off, shrinks, fades. it cleans up after itself

struct ghost_style {
	enum flame_anim_type type;
	bool fade; // on top of a slide or zoom, from fade_from
	float fade_from;
	float zoom_to;
	double slide_dx, slide_dy; // where a slide ends up
	int ms;
	struct flame_curve curve;
};

// the ghost takes texture, it's destroyed with it
void ghost_play(struct flame_anims *anims, struct flame_tree *parent,
	struct wlr_texture *texture, struct wlr_box box, const struct ghost_style *style);

#endif
