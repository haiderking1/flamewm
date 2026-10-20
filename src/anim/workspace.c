#include "anim/workspace.h"

#include "config/config.h"
#include "core/server.h"
#include "scene/scene.h"

// one switch at a time. each of the two moves from its start offset to its
// end offset: the new one in to 0, the old one out of sight
static struct {
	struct flame_anim anim;
	struct flame_desktop *desktop;
	struct flame_workspace *from, *to;
	double from_start, from_end, to_start;
	double from_now, to_now; // where they are, a switch back starts there
	bool vertical;
} sliding;

static struct flame_tree **trees_of(struct flame_workspace *workspace, struct flame_tree *out[3]) {
	out[0] = workspace->tree;
	out[1] = workspace->float_tree;
	out[2] = workspace->fullscreen_tree;
	return out;
}

static void set_offset(struct flame_workspace *workspace, double offset, bool vertical) {
	struct flame_tree *trees[3];
	trees_of(workspace, trees);
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

static void set_passthrough(struct flame_workspace *workspace, bool passthrough) {
	struct flame_tree *trees[3];
	trees_of(workspace, trees);
	for (int i = 0; i < 3; i++) {
		trees[i]->node.passthrough = passthrough;
	}
}

static void step(struct flame_anim *anim, double eased, double t) {
	sliding.to_now = lerp(sliding.to_start, 0, eased);
	sliding.from_now = lerp(sliding.from_start, sliding.from_end, eased);
	set_offset(sliding.to, sliding.to_now, sliding.vertical);
	set_offset(sliding.from, sliding.from_now, sliding.vertical);
}

static void done(struct flame_anim *anim, bool finished) {
	struct flame_desktop *desktop = sliding.desktop;
	struct flame_workspace *from = sliding.from, *to = sliding.to;
	sliding.from = NULL;
	sliding.to = NULL;
	if (!from || !to) {
		return;
	}
	set_offset(to, 0, sliding.vertical);
	set_offset(from, 0, sliding.vertical);
	set_passthrough(from, false);
	if (from != desktop->active) {
		workspace_set_visible(from, false);
	}
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
	struct flame_output *output = outputs_primary(&server->outputs);
	bool reversing = anim_running(&sliding.anim) && sliding.from == to && sliding.to == from;
	// going back where it came from mid slide: both turn around from where
	// they are now
	double from_start = 0, to_start = 0;
	if (reversing) {
		from_start = sliding.to_now;
		to_start = sliding.from_now;
		sliding.from = NULL; // so the cut short one leaves them be
		sliding.to = NULL;
		anim_cancel(&sliding.anim);
		set_passthrough(to, false);
	} else {
		workspace_anim_finish(desktop);
	}
	if (!config->enabled || config->workspace_ms <= 0 || !output) {
		set_offset(from, 0, config->workspace_vertical);
		set_offset(to, 0, config->workspace_vertical);
		return false;
	}
	double distance = config->workspace_vertical ? output->box.height : output->box.width;
	int sign = to->index > from->index ? 1 : -1;
	sliding.desktop = desktop;
	sliding.from = from;
	sliding.to = to;
	sliding.vertical = config->workspace_vertical;
	sliding.from_start = from_start;
	sliding.from_end = -sign * distance;
	sliding.to_start = reversing ? to_start : sign * distance;
	sliding.from_now = sliding.from_start;
	sliding.to_now = sliding.to_start;
	sliding.anim.step = step;
	sliding.anim.done = done;
	// both show while they slide, only the new one takes the pointer
	workspace_set_visible(from, true);
	set_passthrough(from, true);
	anim_start(&server->anims, &sliding.anim, config->workspace_ms, &config->workspace_curve);
	return true;
}
