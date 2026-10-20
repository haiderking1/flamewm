#ifndef FLAME_RENDER_RENDER_H
#define FLAME_RENDER_RENDER_H

#include <stdbool.h>

struct flame_server;

// bottom to top
enum flame_layer {
	FLAME_LAYER_BACKGROUND,
	FLAME_LAYER_BOTTOM,
	FLAME_LAYER_TILED,
	FLAME_LAYER_FLOATING,
	FLAME_LAYER_TOP,
	FLAME_LAYER_OVERLAY,
	FLAME_LAYER_COUNT,
};

struct flame_render {
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;
	struct wlr_scene_tree *layers[FLAME_LAYER_COUNT];
};

bool render_init(struct flame_render *render, struct flame_server *server);
void render_finish(struct flame_render *render);

#endif
