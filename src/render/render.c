#include "render/render.h"

#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#include "core/server.h"

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

	render->scene = wlr_scene_create();
	if (!render->scene) {
		wlr_log(WLR_ERROR, "failed to create scene");
		return false;
	}
	render->scene_layout = wlr_scene_attach_output_layout(render->scene,
		server->outputs.layout);

	// dmabuf ourselves instead of wlr_renderer_init_wl_display() so the scene
	// gets the handle and can send scanout feedback to clients
	if (wlr_renderer_get_texture_formats(render->renderer, WLR_BUFFER_CAP_DMABUF)) {
		struct wlr_linux_dmabuf_v1 *dmabuf =
			wlr_linux_dmabuf_v1_create_with_renderer(server->display, 5, render->renderer);
		if (dmabuf) {
			wlr_scene_set_linux_dmabuf_v1(render->scene, dmabuf);
		}
	}

	// explicit sync: clients say when the gpu is done with a buffer instead of
	// us putting it on screen half drawn. nvidia has no implicit sync, so
	// without this a frame can show up before it's rendered (its background
	// color flashing through), the scene waits on these timelines for us
	int drm_fd = wlr_renderer_get_drm_fd(render->renderer);
	if (render->renderer->features.timeline && server->backend.wlr->features.timeline
			&& drm_fd >= 0
			&& wlr_linux_drm_syncobj_manager_v1_create(server->display, 1, drm_fd)) {
		wlr_log(WLR_INFO, "explicit sync (linux-drm-syncobj-v1) enabled");
	} else {
		wlr_log(WLR_INFO, "explicit sync not available (renderer timeline=%d backend timeline=%d)",
			render->renderer->features.timeline, server->backend.wlr->features.timeline);
	}

	for (int i = 0; i < FLAME_LAYER_COUNT; i++) {
		render->layers[i] = wlr_scene_tree_create(&render->scene->tree);
	}
	return true;
}

void render_finish(struct flame_render *render) {
	wlr_scene_node_destroy(&render->scene->tree.node);
	wlr_allocator_destroy(render->allocator);
	wlr_renderer_destroy(render->renderer);
}
