#include "render/render.h"

#include <stdlib.h>
#include <string.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_tearing_control_v1.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/util/log.h>

#include "core/server.h"
#include "cursor/image.h"
#include "render/timing.h"
#include "util/listener.h"

static void pointer_changed(struct flame_render *render) {
	wl_signal_emit_mutable(&render->pointer.events.changed, NULL);
}

static void handle_pointer_image_changed(struct wl_listener *listener, void *data) {
	struct flame_render *render = wl_container_of(listener, render, pointer.image_changed);
	pointer_changed(render);
}

void render_pointer_set_image(struct flame_render *render, struct flame_cursor_image *image) {
	struct flame_render_pointer *pointer = &render->pointer;
	if (pointer->image) {
		listener_disconnect(&pointer->image_changed);
	}
	pointer->image = image;
	if (image) {
		listener_connect(&pointer->image_changed, &image->events.changed,
			handle_pointer_image_changed);
	}
	pointer_changed(render);
}

void render_pointer_move(struct flame_render *render, double x, double y) {
	if (render->pointer.x == x && render->pointer.y == y) {
		return;
	}
	render->pointer.x = x;
	render->pointer.y = y;
	pointer_changed(render);
}

bool render_init(struct flame_render *render, struct flame_server *server) {
	render->server = server;
	render->renderer = wlr_renderer_autocreate(server->backend.wlr);
	if (!render->renderer) {
		wlr_log(WLR_ERROR, "failed to create renderer");
		return false;
	}
	// flame draws its frames itself. FLAME_RENDERER=wlroots leaves it to
	// wlroots' render pass: to compare against, or to get around a bug of ours
	const char *drawer = getenv("FLAME_RENDERER");
	if (!drawer || strcmp(drawer, "wlroots")) {
		gl_init(&render->gl, render->renderer);
	} else {
		render->gl = (struct flame_gl){0};
		wlr_log(WLR_INFO, "FLAME_RENDERER=wlroots: wlroots draws the frames");
	}
	if (!wlr_renderer_init_wl_shm(render->renderer, server->display)) {
		wlr_log(WLR_ERROR, "failed to set up wl_shm");
		return false;
	}

	render->allocator = wlr_allocator_autocreate(server->backend.wlr, render->renderer);
	if (!render->allocator) {
		wlr_log(WLR_ERROR, "failed to create allocator");
		return false;
	}

	render->compositor = wlr_compositor_create(server->display, 6, render->renderer);
	if (!render->compositor) {
		wlr_log(WLR_ERROR, "failed to create compositor");
		return false;
	}
	scene_init(&render->scene);
	// every surface gets tracked from birth, so even ones we haven't drawn
	// yet wake the outputs when they show up
	scene_surfaces_init(&render->surfaces, &render->scene, render->compositor);
	acquire_init(&render->acquire, server->loop, render->compositor);

	// clients get the formats our renderer can import, default feedback only.
	// scanout hints come back with direct scanout
	if (wlr_renderer_get_texture_formats(render->renderer, WLR_BUFFER_CAP_DMABUF)) {
		render->dmabuf =
			wlr_linux_dmabuf_v1_create_with_renderer(server->display, 5, render->renderer);
	}

	// fullscreen games ask to tear (vsync off) through this, see frame.c
	render->tearing = wlr_tearing_control_manager_v1_create(server->display, 1);
	if (!render->tearing) {
		wlr_log(WLR_ERROR, "failed to create the tearing control manager");
		return false;
	}

	// explicit sync: clients say when the gpu is done with a buffer instead of
	// us putting it on screen half drawn. nvidia has no implicit sync, so
	// without this a frame can show up before it's rendered (its background
	// color flashing through), frame.c waits on these timelines for us
	int drm_fd = wlr_renderer_get_drm_fd(render->renderer);
	if (render->renderer->features.timeline && server->backend.wlr->features.timeline
			&& drm_fd >= 0
			&& wlr_linux_drm_syncobj_manager_v1_create(server->display, 1, drm_fd)) {
		wlr_log(WLR_INFO, "explicit sync (linux-drm-syncobj-v1) enabled");
	} else {
		wlr_log(WLR_INFO, "explicit sync not available (renderer timeline=%d backend timeline=%d)",
			render->renderer->features.timeline, server->backend.wlr->features.timeline);
	}

	// a typo here would quietly test the wrong thing, so it stops startup
	const char *pipeline = getenv("FLAME_FRAME_PIPELINE");
	if (!schedule_policy_parse(pipeline, &render->pipeline)) {
		wlr_log(WLR_ERROR, "FLAME_FRAME_PIPELINE=%s: use adaptive, immediate or deadline",
			pipeline);
		return false;
	}
	wlr_log(WLR_INFO, "frame pipeline: %s", schedule_policy_name(render->pipeline));

	// 0 draws the pointer into every frame, for comparing or a broken plane
	const char *hardware_cursor = getenv("FLAME_HARDWARE_CURSOR");
	if (hardware_cursor && strcmp(hardware_cursor, "0") && strcmp(hardware_cursor, "1")) {
		wlr_log(WLR_ERROR, "FLAME_HARDWARE_CURSOR=%s: use 0 or 1", hardware_cursor);
		return false;
	}
	render->hardware_cursor = !hardware_cursor || !strcmp(hardware_cursor, "1");
	if (!render->hardware_cursor) {
		wlr_log(WLR_INFO, "hardware cursor off, the pointer is drawn into frames");
	}
	wl_signal_init(&render->pointer.events.changed);

	render->frame_timing = timing_enabled_by_env();
	if (render->frame_timing) {
		wlr_log(WLR_INFO, "frame timing probes on, reports every 2s");
	}

	for (int i = 0; i < FLAME_LAYER_COUNT; i++) {
		render->layers[i] = scene_tree_create(&render->scene.root);
		if (!render->layers[i]) {
			wlr_log(WLR_ERROR, "out of memory");
			return false;
		}
	}
	return true;
}

void render_unhook(struct flame_render *render) {
	scene_surfaces_finish(&render->surfaces);
	acquire_finish(&render->acquire);
}

void render_finish(struct flame_render *render) {
	scene_finish(&render->scene);
	wlr_allocator_destroy(render->allocator);
	gl_finish(&render->gl);
	wlr_renderer_destroy(render->renderer);
}
