#include "anim/workspace.h"

#include "config/config.h"
#include "core/server.h"
#include "scene/scene.h"

// one switch at a time: a new one lands the last first
static struct {
	struct flame_anim anim;
	struct flame_desktop *desktop;
	struct flame_workspace *from, *to;
	double distance; // how far one slides, the output's width or height
	int sign; // which way: +1 when going up in number
	bool vertical;
} sliding;

static void set_offset(struct flame_workspace *workspace, double offset, bool vertical) {
	struct flame_tree *trees[] = {workspace->tree, workspace->float_tree,
		workspace->fullscreen_tree};
	for (int i = 0; i < 3; i++) {
		if (offset == 0) {
			scene_tree_set_effect(trees[i], NULL);
			continue;
		}
		struct flame_effect e = {
			.opacity = 1,
			.scale_x = 1,
			.scale_y = 1,
			.dx = vertical ? 0 : offset,
			.dy = vertical ? offset : 0,
		};
		scene_tree_set_effect(trees[i], &e);
	}
}

static void step(struct flame_anim *anim, double eased, double t) {
	double travel = sliding.distance * eased;
	// the new one comes in from the side the numbers go, the old one leaves
	// the other way
	set_offset(sliding.to, sliding.sign * (sliding.distance - travel), sliding.vertical);
	set_offset(sliding.from, -sliding.sign * travel, sliding.vertical);
}

static void done(struct flame_anim *anim, bool finished) {
	struct flame_desktop *desktop = sliding.desktop;
	set_offset(sliding.to, 0, sliding.vertical);
	set_offset(sliding.from, 0, sliding.vertical);
	if (sliding.from != desktop->active) {
		workspace_set_visible(sliding.from, false);
	}
	sliding.from = NULL;
	sliding.to = NULL;
}

void workspace_anim_finish(struct flame_desktop *desktop) {
	if (anim_running(&sliding.anim)) {
		anim_finish(&sliding.anim);
	}
}

bool workspace_anim_switch(struct flame_desktop *desktop, struct flame_workspace *from,
		struct flame_workspace *to) {
	struct flame_server *server = desktop->server;
	const struct flame_anim_config *config = &server->config->anim;
	workspace_anim_finish(desktop);
	struct flame_output *output = outputs_primary(&server->outputs);
	if (!config->enabled || config->workspace_ms <= 0 || !output) {
		return false;
	}
	sliding.desktop = desktop;
	sliding.from = from;
	sliding.to = to;
	sliding.vertical = config->workspace_vertical;
	sliding.distance = sliding.vertical ? output->box.height : output->box.width;
	sliding.sign = to->index > from->index ? 1 : -1;
	sliding.anim.step = step;
	sliding.anim.done = done;
	// both show while they slide
	workspace_set_visible(from, true);
	anim_start(&server->anims, &sliding.anim, config->workspace_ms, &config->workspace_curve);
	return true;
}
