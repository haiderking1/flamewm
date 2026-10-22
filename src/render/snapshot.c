#include "render/snapshot.h"

#include <drm_fourcc.h>
#include <math.h>
#include <string.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>
#include <wlr/util/transform.h>

#include "render/coords.h"
#include "render/elements.h"
#include "render/frame.h"
#include "render/pass.h"
#include "render/render.h"
#include "scene/scene.h"

// a picture of part of the scene in a texture of its own: what a closing
// window looked like, to animate after the client let go of its buffers.
// drawn into output sized buffers kept for this, at the output's scale: the
// gpu side of a buffer (its framebuffer, its texture) is set up once and
// reused, a close costs a few draw calls and nothing else

// the output's size in pixels, untransformed: snapshots are drawn upright
static void pixel_size(struct flame_frame *frame, int *width, int *height) {
	float scale = frame->output->scale > 0 ? frame->output->scale : 1;
	*width = (int)ceil(frame->area->width * scale);
	*height = (int)ceil(frame->area->height * scale);
}

static bool same_format(const struct wlr_drm_format *a, const struct wlr_drm_format *b) {
	return a->format == b->format && a->len == b->len
		&& !memcmp(a->modifiers, b->modifiers, a->len * sizeof(*a->modifiers));
}

// the format and modifiers the output's own frames are drawn into: known to
// work with this gpu (nvidia draws into neither linear nor implicit)
static struct wlr_swapchain *swapchain(struct flame_frame *frame) {
	struct wlr_output *output = frame->output;
	int width, height;
	pixel_size(frame, &width, &height);
	if (!output->swapchain || width <= 0 || height <= 0) {
		return NULL;
	}
	struct wlr_swapchain *chain = frame->snapshots;
	if (chain && (chain->width != width || chain->height != height
			|| !same_format(&chain->format, &output->swapchain->format))) {
		// ghosts still showing keep their buffers, this lets go of the rest
		wlr_swapchain_destroy(chain);
		chain = NULL;
	}
	if (!chain) {
		chain = wlr_swapchain_create(frame->render->allocator, width, height,
			&output->swapchain->format);
	}
	frame->snapshots = chain;
	return chain;
}

void render_snapshot_warm(struct flame_frame *frame) {
	struct wlr_swapchain *chain = swapchain(frame);
	if (!chain || chain->slots[0].buffer) {
		return; // ready, or nothing to draw with
	}
	// one buffer through both its framebuffer and its texture now, so the
	// first close doesn't pay for it
	struct wlr_buffer *buffer = wlr_swapchain_acquire(chain);
	if (!buffer) {
		return;
	}
	struct wlr_renderer *renderer = frame->render->renderer;
	struct flame_pass pass;
	if (pass_begin(&pass, frame->render, buffer, NULL, 0)) {
		pass_submit(&pass);
	}
	wlr_texture_destroy(wlr_texture_from_buffer(renderer, buffer));
	wlr_buffer_unlock(buffer);
}

static void draw(struct flame_pass *pass, const struct flame_render_target *target,
		const struct flame_element_list *list) {
	for (size_t i = 0; i < list->len; i++) {
		const struct flame_element *element = &list->data[i];
		struct wlr_box dst = element->box;
		target_box_to_buffer(target, &dst);
		pixman_region32_t clip;
		pixman_region32_init_rect(&clip, element->clip.x, element->clip.y,
			(unsigned)element->clip.width, (unsigned)element->clip.height);
		target_region_to_buffer(target, &clip, true);
		if (element->type == FLAME_ELEMENT_RECT) {
			pass_rect(pass, dst, element->color, &clip, true);
		} else if (element->type == FLAME_ELEMENT_GLASS) {
			pass_glass_style(pass, &element->glass, dst, &clip, target->scale, 1);
		} else if (element->texture) {
			struct wlr_linux_drm_syncobj_surface_v1_state *sync = element->surface
				? wlr_linux_drm_syncobj_v1_get_surface_state(element->surface) : NULL;
			pass_texture(pass, &(struct gl_texture_options){
				.texture = element->texture,
				.src = element->src,
				.dst = dst,
				.transform = wlr_output_transform_invert(element->transform),
				.clip = &clip,
				.alpha = 1,
				.blend = true,
				.wait_timeline = sync ? sync->acquire_timeline : element->wait_timeline,
				.wait_point = sync ? sync->acquire_point : element->wait_point,
			});
		}
		pixman_region32_fini(&clip);
	}
}

bool render_elements_to_buffer(struct flame_render *render,
		const struct flame_element_list *list, struct wlr_box area, float scale,
		struct wlr_buffer *buffer) {
	struct flame_render_target target = {
		.logical = area,
		.scale = scale,
		.transform = WL_OUTPUT_TRANSFORM_NORMAL,
		.trans_width = buffer->width,
		.trans_height = buffer->height,
	};
	struct flame_pass pass;
	if (!pass_begin(&pass, render, buffer, NULL, 0)) {
		return false;
	}
	pass_rect(&pass, (struct wlr_box){0, 0, buffer->width, buffer->height},
		(const float[4]){0, 0, 0, 0}, NULL, false);
	draw(&pass, &target, list);
	return pass_submit(&pass);
}

struct wlr_texture *render_snapshot(struct flame_frame *frame, struct flame_node *node,
		struct wlr_box *box, struct wlr_fbox *src) {
	struct wlr_box area;
	struct wlr_swapchain *chain = swapchain(frame);
	if (!chain || !wlr_box_intersection(&area, box, frame->area)) {
		return NULL;
	}
	// all of them showing ghosts (a lot closing at once): one of its own
	bool kept = true;
	struct wlr_buffer *buffer = wlr_swapchain_acquire(chain);
	if (!buffer) {
		kept = false;
		buffer = wlr_allocator_create_buffer(frame->render->allocator, chain->width,
			chain->height, &chain->format);
		if (!buffer) {
			return NULL;
		}
	}
	struct flame_render_target target = {
		.logical = *frame->area,
		.scale = frame->output->scale > 0 ? frame->output->scale : 1,
		.transform = WL_OUTPUT_TRANSFORM_NORMAL,
		.trans_width = chain->width,
		.trans_height = chain->height,
	};
	struct wlr_box shown = area;
	target_box_to_buffer(&target, &shown);

	struct flame_element_list list;
	elements_init(&list);
	bool ok = elements_collect_node(&list, node, area);
	struct wlr_renderer *renderer = frame->render->renderer;
	struct flame_pass pass;
	ok = ok && pass_begin(&pass, frame->render, buffer, NULL, 0);
	if (ok) {
		// only its part of the buffer is ever shown
		pass_rect(&pass, shown, (const float[4]){0, 0, 0, 0}, NULL, false);
		draw(&pass, &target, &list);
		ok = pass_submit(&pass);
	}
	elements_finish(&list);
	struct wlr_texture *texture = ok ? wlr_texture_from_buffer(renderer, buffer) : NULL;
	// the texture holds on to the buffer while a ghost shows it, the
	// swapchain hands it out again after
	if (kept) {
		wlr_buffer_unlock(buffer);
	} else {
		wlr_buffer_drop(buffer);
	}
	if (!texture) {
		return NULL;
	}
	*box = area;
	*src = (struct wlr_fbox){shown.x, shown.y, shown.width, shown.height};
	return texture;
}
