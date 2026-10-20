#include "render/render.h"

#include <stdlib.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/util/log.h>

#include "core/server.h"
#include "render/timing.h"

bool render_init(struct flame_render *render, struct flame_server *server) {
	render->renderer = wlr_renderer_autocreate(server->backend.wlr);
	if (!render->renderer) {
		wlr_log(WLR_ERROR, "failed to create renderer");
		return false;
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
	wlr_renderer_destroy(render->renderer);
}
