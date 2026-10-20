#include "render/snapshot.h"

#include <drm_fourcc.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>
#include <wlr/util/transform.h>

#include "render/elements.h"
#include "render/render.h"
#include "scene/scene.h"

// a picture of part of the scene in a texture of its own: what a closing
// window looked like, to animate after the client let go of its buffers

struct wlr_texture *render_snapshot(struct flame_render *render, struct wlr_output *output,
		struct flame_node *node, struct wlr_box box) {
	if (wlr_box_empty(&box) || !output) {
			return NULL;
	}
	// the format and modifiers the output's own frames are drawn into: known
	// to work with this gpu (nvidia draws into neither linear nor implicit)
	if (!output->swapchain) {
		return NULL;
	}
	const struct wlr_drm_format *format = &output->swapchain->format;
	struct wlr_buffer *buffer = wlr_allocator_create_buffer(render->allocator, box.width,
		box.height, format);
	if (!buffer) {
			return NULL;
	}
	struct flame_element_list list;
	elements_init(&list);
	// the node's own position is where box starts
	struct wlr_box area = {node->x, node->y, box.width, box.height};
	bool ok = elements_collect_node(&list, node, area);
	struct wlr_render_pass *pass = ok
		? wlr_renderer_begin_buffer_pass(render->renderer, buffer, NULL) : NULL;
	if (pass) {
		wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
			.box = {0, 0, box.width, box.height},
			.color = {0, 0, 0, 0},
			.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
		});
		for (size_t i = 0; i < list.len; i++) {
			const struct flame_element *element = &list.data[i];
			struct wlr_box dst = element->box;
			dst.x -= area.x;
			dst.y -= area.y;
			pixman_region32_t clip;
			pixman_region32_init_rect(&clip, element->clip.x - area.x, element->clip.y - area.y,
				(unsigned)element->clip.width, (unsigned)element->clip.height);
			if (element->type == FLAME_ELEMENT_RECT) {
				wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
					.box = dst,
					.color = {element->color[0], element->color[1], element->color[2],
						element->color[3]},
					.clip = &clip,
				});
			} else if (element->texture) {
				struct wlr_linux_drm_syncobj_surface_v1_state *sync = element->surface
					? wlr_linux_drm_syncobj_v1_get_surface_state(element->surface) : NULL;
				wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
					.texture = element->texture,
					.src_box = element->src,
					.dst_box = dst,
					.transform = wlr_output_transform_invert(element->transform),
					.clip = &clip,
					.blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
					.wait_timeline = sync ? sync->acquire_timeline : element->wait_timeline,
					.wait_point = sync ? sync->acquire_point : element->wait_point,
				});
			}
			pixman_region32_fini(&clip);
		}
		ok = wlr_render_pass_submit(pass);
	}
	elements_finish(&list);
	struct wlr_texture *texture = ok && pass
		? wlr_texture_from_buffer(render->renderer, buffer) : NULL;
	// the texture holds on to what it needs
	wlr_buffer_drop(buffer);
	return texture;
}
