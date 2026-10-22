#include <cairo.h>
#include <drm_fourcc.h>
#include <math.h>
#include <wlr/render/allocator.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

#include "core/server.h"
#include "desktop/view.h"
#include "render/elements.h"
#include "render/snapshot.h"
#include "screencast/screencast.h"

// a still: what the target shows drawn once into a gpu buffer the screen's
// format, read back into cairo's memory (premultiplied ARGB, the same bytes
// as DRM's ARGB8888 here), cairo writes the png

bool screencast_shoot(struct flame_screencast *screencast, const struct cast_target *target,
		const char *path) {
	struct flame_server *server = screencast->server;
	struct flame_output *screen = target->output ? target->output
		: outputs_primary(&server->outputs);
	if (!screen || !screen->wlr->swapchain) {
		return false;
	}
	float scale = screen->wlr->scale > 0 ? screen->wlr->scale : 1;
	struct wlr_box area;
	struct flame_element_list list;
	elements_init(&list);
	bool ok;
	if (target->view) {
		int width = 0, height = 0;
		target->view->impl->content_size(target->view, &width, &height);
		struct flame_node *node = &target->view->content->node;
		area = (struct wlr_box){node->x, node->y, width, height};
		ok = width > 0 && height > 0 && elements_collect_node(&list, node, area);
	} else {
		area = screen->box;
		ok = elements_collect(&list, &server->render.scene, area);
	}
	int width = (int)ceilf(area.width * scale), height = (int)ceilf(area.height * scale);
	struct wlr_buffer *buffer = ok ? wlr_allocator_create_buffer(server->render.allocator,
		width, height, &screen->wlr->swapchain->format) : NULL;
	ok = buffer && render_elements_to_buffer(&server->render, &list, area, scale, buffer, NULL,
		0);
	elements_finish(&list);
	cairo_surface_t *image = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	struct wlr_texture *texture = ok
		? wlr_texture_from_buffer(server->render.renderer, buffer) : NULL;
	ok = texture && cairo_surface_status(image) == CAIRO_STATUS_SUCCESS
		&& wlr_texture_read_pixels(texture, &(struct wlr_texture_read_pixels_options){
			.data = cairo_image_surface_get_data(image),
			.format = DRM_FORMAT_ARGB8888,
			.stride = (uint32_t)cairo_image_surface_get_stride(image),
		});
	if (texture) {
		wlr_texture_destroy(texture);
	}
	if (buffer) {
		wlr_buffer_drop(buffer);
	}
	if (ok) {
		cairo_surface_mark_dirty(image);
		ok = cairo_surface_write_to_png(image, path) == CAIRO_STATUS_SUCCESS;
	}
	cairo_surface_destroy(image);
	if (!ok) {
		wlr_log(WLR_ERROR, "screencast: no screenshot at %s", path);
	}
	return ok;
}
