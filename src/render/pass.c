#include "render/pass.h"

#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>

#include "render/render.h"

bool pass_begin(struct flame_pass *pass, struct flame_render *render,
		struct wlr_buffer *buffer, struct wlr_drm_syncobj_timeline *signal, uint64_t point) {
	pass->wlr = NULL;
	if (render->gl.usable) {
		return gl_pass_begin(&pass->gl, &render->gl, buffer, signal, point);
	}
	pass->wlr = wlr_renderer_begin_buffer_pass(render->renderer, buffer,
		&(struct wlr_buffer_pass_options){
			.signal_timeline = signal,
			.signal_point = point,
		});
	return pass->wlr != NULL;
}

void pass_rect(struct flame_pass *pass, struct wlr_box box, const float color[4],
		const pixman_region32_t *clip, bool blend) {
	if (!pass->wlr) {
		gl_pass_rect(&pass->gl, box, color, clip, blend);
		return;
	}
	wlr_render_pass_add_rect(pass->wlr, &(struct wlr_render_rect_options){
		.box = box,
		.color = {color[0], color[1], color[2], color[3]},
		.clip = clip,
		.blend_mode = blend ? WLR_RENDER_BLEND_MODE_PREMULTIPLIED
			: WLR_RENDER_BLEND_MODE_NONE,
	});
}

void pass_texture(struct flame_pass *pass, const struct gl_texture_options *options) {
	if (!pass->wlr) {
		gl_pass_texture(&pass->gl, options);
		return;
	}
	wlr_render_pass_add_texture(pass->wlr, &(struct wlr_render_texture_options){
		.texture = options->texture,
		.src_box = options->src,
		.dst_box = options->dst,
		.transform = options->transform,
		.clip = options->clip,
		.alpha = options->alpha < 1.0f ? &options->alpha : NULL,
		.blend_mode = options->blend ? WLR_RENDER_BLEND_MODE_PREMULTIPLIED
			: WLR_RENDER_BLEND_MODE_NONE,
		.filter_mode = options->nearest ? WLR_SCALE_FILTER_NEAREST : WLR_SCALE_FILTER_BILINEAR,
		.wait_timeline = options->wait_timeline,
		.wait_point = options->wait_point,
	});
}

bool pass_submit(struct flame_pass *pass) {
	if (!pass->wlr) {
		return gl_pass_submit(&pass->gl);
	}
	return wlr_render_pass_submit(pass->wlr);
}
