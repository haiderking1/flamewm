#ifndef FLAME_RENDER_RENDER_H
#define FLAME_RENDER_RENDER_H

#include <stdbool.h>

#include "gl/gl.h"
#include "render/acquire.h"
#include "render/schedule.h"
#include "scene/scene.h"
#include "scene/surface.h"

struct flame_cursor_image;
struct flame_server;

// the mouse pointer as outputs show it: where the hotspot is (layout coords)
// and what it looks like. input/ keeps it current, outputs listen to changed
struct flame_render_pointer {
	struct flame_cursor_image *image; // NULL: no pointer
	double x, y;
	struct wl_listener image_changed;
	struct {
		struct wl_signal changed;
	} events;
};

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
// flame decides what to draw and when (frame.c) and draws it itself (gl/),
// wlroots' renderer turns client buffers into textures for it
struct flame_render {
	struct flame_server *server;
	struct wlr_renderer *renderer;
	// flame's own drawing, in renderer's GL context. unusable without GL:
	// wlroots' render pass draws then, see render/pass.h
	struct flame_gl gl;
	struct wlr_allocator *allocator;
	// client buffers turn into textures for our renderer here
	struct wlr_compositor *compositor;
	struct wlr_linux_dmabuf_v1 *dmabuf; // NULL without dmabuf support
	// apps saying a frame may tear (wp_tearing_control_v1)
	struct wlr_tearing_control_manager_v1 *tearing;

	struct flame_scene scene;
	struct flame_scene_surfaces surfaces;
	struct flame_acquire acquire; // client buffers wait for their gpu work
	struct flame_tree *layers[FLAME_LAYER_COUNT];
	struct flame_render_pointer pointer;

	bool frame_timing; // FLAME_FRAME_TIMING=1, frame.c logs probes
	enum schedule_policy pipeline; // FLAME_FRAME_PIPELINE, adaptive by default
	bool hardware_cursor; // FLAME_HARDWARE_CURSOR, on by default
};

bool render_init(struct flame_render *render, struct flame_server *server);
// what the pointer looks like from now on, NULL for nothing
void render_pointer_set_image(struct flame_render *render, struct flame_cursor_image *image);
void render_pointer_move(struct flame_render *render, double x, double y);
// before the display goes: stop listening to things that die with it
void render_unhook(struct flame_render *render);
// after the display, nothing references the scene or renderer anymore
void render_finish(struct flame_render *render);

#endif
