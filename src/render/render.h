#ifndef FLAME_RENDER_RENDER_H
#define FLAME_RENDER_RENDER_H

#include <stdbool.h>

#include "scene/scene.h"
#include "scene/surface.h"

struct flame_server;

// bottom to top
enum flame_layer {
	FLAME_LAYER_BACKGROUND,
	FLAME_LAYER_BOTTOM,
	FLAME_LAYER_TILED,
	FLAME_LAYER_FLOATING,
	FLAME_LAYER_TOP,
	FLAME_LAYER_FULLSCREEN,
	// layer surfaces holding the keyboard (launchers), above fullscreen
	FLAME_LAYER_GRAB,
	FLAME_LAYER_OVERLAY,
	FLAME_LAYER_COUNT,
};

// the gpu side: renderer, buffers, and the scene everything gets drawn from.
// flame decides what to draw and when (frame.c), the wlroots renderer only
// gets told "this texture here" and "this rect there"
struct flame_render {
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	// client buffers turn into textures for our renderer here
	struct wlr_compositor *compositor;
	struct wlr_linux_dmabuf_v1 *dmabuf; // NULL without dmabuf support

	struct flame_scene scene;
	struct flame_scene_surfaces surfaces;
	struct flame_tree *layers[FLAME_LAYER_COUNT];
};

bool render_init(struct flame_render *render, struct flame_server *server);
// before the display goes: stop listening to things that die with it
void render_unhook(struct flame_render *render);
// after the display, nothing references the scene or renderer anymore
void render_finish(struct flame_render *render);

#endif
