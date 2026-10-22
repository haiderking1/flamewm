#include "render/cursor.h"

#include <drm_fourcc.h>
#include <math.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>
#include <wlr/xcursor.h>

#include "cursor/image.h"
#include "render/coords.h"
#include "render/elements.h"
#include "render/render.h"

void frame_cursor_init(struct flame_frame_cursor *cursor, struct flame_render *render,
		struct wlr_output *output) {
	cursor->render = render;
	cursor->output = output;
	if (render->hardware_cursor) {
		cursor_plane_init(&cursor->plane, output);
	} else {
		cursor->plane = (struct flame_cursor_plane){.output = output, .device.fd = -1};
	}
	if (cursor->plane.usable) {
		wlr_log(WLR_INFO, "%s: pointer on the cursor plane", output->name);
	}
}

static void drop_texture(struct flame_frame_cursor *cursor) {
	if (cursor->texture) {
		wlr_texture_destroy(cursor->texture);
		cursor->texture = NULL;
		cursor->texture_image = NULL;
	}
}

void frame_cursor_finish(struct flame_frame_cursor *cursor) {
	cursor_plane_finish(&cursor->plane);
	drop_texture(cursor);
	cursor->plane_surface = NULL;
}

// a client's cursor fits the plane when its pixels map 1:1 onto the output's
static bool surface_fits_plane(struct wlr_surface *surface, struct wlr_texture *texture,
		float scale) {
	return surface->current.transform == WL_OUTPUT_TRANSFORM_NORMAL
		&& wl_list_empty(&surface->current.subsurfaces_below)
		&& wl_list_empty(&surface->current.subsurfaces_above)
		&& (int)texture->width == (int)lroundf(surface->current.width * scale)
		&& (int)texture->height == (int)lroundf(surface->current.height * scale);
}

static bool show_on_plane(struct flame_frame_cursor *cursor,
		const struct flame_cursor_frame *frame, uint32_t generation) {
	struct flame_cursor_plane *plane = &cursor->plane;
	struct wlr_output *output = cursor->output;
	if (!plane->usable || output->transform != WL_OUTPUT_TRANSFORM_NORMAL
			|| output->software_cursor_locks > 0) {
		// a recorder asking for the cursor in its frames locks it to software
		return false;
	}
	if (frame->kind != FLAME_CURSOR_SURFACE) {
		// the plane shows buffer pixels 1:1
		const struct wlr_xcursor_image *image = frame->xcursor;
		if (frame->scale != output->scale) {
			return false;
		}
		// an X client's pixels can come back at a freed one's address
		uint64_t seq = frame->kind == FLAME_CURSOR_PIXELS ? generation : 0;
		return cursor_plane_show_pixels(plane, image, seq, image->buffer, (int)image->width,
			(int)image->height, (int)image->width * 4, frame->hotspot_x, frame->hotspot_y);
	}
	struct wlr_texture *texture = wlr_surface_get_texture(frame->surface);
	if (!texture || !surface_fits_plane(frame->surface, texture, output->scale)) {
		return false;
	}
	// a commit or a new set_cursor makes it a different image
	uint64_t seq = (uint64_t)generation << 32 | frame->surface->current.seq;
	return cursor_plane_show_texture(plane, frame->surface, seq, texture,
		(int)lroundf(frame->hotspot_x * output->scale),
		(int)lroundf(frame->hotspot_y * output->scale));
}

static bool push_software(struct flame_frame_cursor *cursor,
		const struct flame_cursor_frame *frame, uint32_t generation, double x, double y,
		struct wlr_box area, struct flame_element_list *list) {
	if (frame->kind == FLAME_CURSOR_SURFACE) {
		return elements_push_surface(list, frame->surface,
			(int)floor(x) - frame->hotspot_x, (int)floor(y) - frame->hotspot_y, area);
	}
	const struct wlr_xcursor_image *image = frame->xcursor;
	// an X client's pixels can come back at a freed one's address
	uint32_t texture_generation = frame->kind == FLAME_CURSOR_PIXELS ? generation : 0;
	if (cursor->texture_image != image || cursor->texture_generation != texture_generation) {
		// made before the old one goes: a new address, damage sees it change
		struct wlr_texture *texture = wlr_texture_from_pixels(cursor->render->renderer,
			DRM_FORMAT_ARGB8888, image->width * 4, image->width, image->height, image->buffer);
		drop_texture(cursor);
		if (!texture) {
			return true; // no pointer this frame, nothing else breaks
		}
		cursor->texture = texture;
		cursor->texture_image = image;
		cursor->texture_generation = texture_generation;
	}
	// its pixels a layout unit: a theme's at the output's scale, X's at X's
	float scale = frame->scale;
	struct wlr_box box = {
		.x = (int)floor(x - frame->hotspot_x / scale),
		.y = (int)floor(y - frame->hotspot_y / scale),
		.width = (int)ceilf(image->width / scale),
		.height = (int)ceilf(image->height / scale),
	};
	return elements_push_texture(list, cursor, cursor->texture, cursor->texture, box, area);
}

bool frame_cursor_build(struct flame_frame_cursor *cursor,
		const struct flame_render_target *target, struct flame_element_list *list) {
	struct flame_render_pointer *pointer = &cursor->render->pointer;
	struct wlr_output *output = cursor->output;
	struct wlr_box area = target->logical;
	cursor->plane_surface = NULL;

	struct flame_cursor_frame frame = {.kind = FLAME_CURSOR_HIDDEN};
	if (pointer->image) {
		cursor_image_get(pointer->image, output->scale, &frame);
	}
	// the hotspot on this output, or close enough for part of the image to be
	bool near = pointer->x >= area.x - 256 && pointer->x < area.x + area.width + 256
		&& pointer->y >= area.y - 256 && pointer->y < area.y + area.height + 256;
	if (frame.kind == FLAME_CURSOR_HIDDEN || !near) {
		cursor_plane_hide(&cursor->plane);
		return true;
	}

	if (show_on_plane(cursor, &frame, pointer->image->generation)) {
		cursor_plane_move(&cursor->plane, (int)floor((pointer->x - area.x) * output->scale),
			(int)floor((pointer->y - area.y) * output->scale));
		if (frame.kind == FLAME_CURSOR_SURFACE) {
			cursor->plane_surface = frame.surface;
		}
		return true;
	}
	cursor_plane_hide(&cursor->plane);
	return push_software(cursor, &frame, pointer->image->generation, pointer->x, pointer->y, area,
		list);
}

bool frame_cursor_push(struct flame_frame_cursor *cursor, struct wlr_box area,
		struct flame_element_list *list) {
	struct flame_render_pointer *pointer = &cursor->render->pointer;
	struct flame_cursor_frame frame = {.kind = FLAME_CURSOR_HIDDEN};
	if (pointer->image) {
		cursor_image_get(pointer->image, cursor->output->scale, &frame);
	}
	if (frame.kind == FLAME_CURSOR_HIDDEN) {
		return true;
	}
	return push_software(cursor, &frame, pointer->image->generation, pointer->x, pointer->y, area,
		list);
}

struct wlr_surface *frame_cursor_surface(struct flame_frame_cursor *cursor) {
	struct flame_cursor_image *image = cursor->render->pointer.image;
	// it may have died since the frame was built
	if (!cursor->plane_surface || !image || image->kind != FLAME_CURSOR_SURFACE
			|| image->surface != cursor->plane_surface) {
		return NULL;
	}
	return cursor->plane_surface;
}

bool frame_cursor_dirty(const struct flame_frame_cursor *cursor) {
	return cursor->plane.dirty;
}

void frame_cursor_committed(struct flame_frame_cursor *cursor) {
	cursor->plane.dirty = false;
}

void frame_cursor_reset(struct flame_frame_cursor *cursor) {
	cursor_plane_forget(&cursor->plane);
}
