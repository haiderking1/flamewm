#include "cursor/plane.h"

#include <drm_fourcc.h>
#include <string.h>
#include <wlr/backend.h>
#include <wlr/backend/drm.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/util/log.h>

bool cursor_plane_init(struct flame_cursor_plane *plane, struct wlr_output *output) {
	memset(plane, 0, sizeof(*plane));
	plane->output = output;
	plane->device.fd = -1;
	if (!wlr_output_is_drm(output) || !output->impl->set_cursor
			|| !output->impl->move_cursor || !output->impl->get_cursor_sizes) {
		return false;
	}
	if (!dumb_device_open(&plane->device, wlr_backend_get_drm_fd(output->backend))) {
		wlr_log(WLR_INFO, "%s: no dumb buffers for the cursor plane, drawing it instead",
			output->name);
		return false;
	}
	plane->usable = true;
	return true;
}

static void drop_buffers(struct flame_cursor_plane *plane) {
	for (int i = 0; i < 3; i++) {
		if (plane->buffers[i]) {
			wlr_buffer_drop(&plane->buffers[i]->base);
			plane->buffers[i] = NULL;
		}
	}
}

void cursor_plane_finish(struct flame_cursor_plane *plane) {
	cursor_plane_hide(plane);
	drop_buffers(plane);
	dumb_device_close(&plane->device);
	plane->usable = false;
}

// the next buffer to write, sized for an image of width x height. NULL when no
// size the plane takes fits it
static struct flame_dumb_buffer *next_buffer(struct flame_cursor_plane *plane,
		int width, int height) {
	struct wlr_output *output = plane->output;
	size_t count = 0;
	const struct wlr_output_cursor_size *sizes = output->impl->get_cursor_sizes(output, &count);
	const struct wlr_output_cursor_size *fit = NULL;
	for (size_t i = 0; i < count; i++) {
		if (width <= sizes[i].width && height <= sizes[i].height) {
			fit = &sizes[i];
			break;
		}
	}
	if (!fit) {
		return NULL;
	}
	struct flame_dumb_buffer *buffer = plane->buffers[plane->next];
	if (buffer && (buffer->base.width != fit->width || buffer->base.height != fit->height)) {
		drop_buffers(plane);
		buffer = NULL;
	}
	if (!buffer) {
		buffer = dumb_buffer_create(&plane->device, fit->width, fit->height);
		if (!buffer) {
			return NULL;
		}
		plane->buffers[plane->next] = buffer;
	}
	// whatever the last image left around the new one
	memset(buffer->data, 0, buffer->size);
	return buffer;
}

static bool hand_over(struct flame_cursor_plane *plane, struct flame_dumb_buffer *buffer,
		const void *key, uint64_t seq, int hotspot_x, int hotspot_y) {
	struct wlr_output *output = plane->output;
	if (!output->impl->set_cursor(output, &buffer->base, hotspot_x, hotspot_y)) {
		// no point trying every frame, it's drawn from now on
		wlr_log(WLR_INFO, "%s: the cursor plane refused our buffer, drawing the cursor instead",
			output->name);
		plane->usable = false;
		plane->shown = NULL;
		return false;
	}
	plane->next = (plane->next + 1) % 3;
	plane->dirty = true;
	plane->shown = key;
	plane->shown_seq = seq;
	// a new hotspot changes where it lands, move it again for this one
	plane->x = plane->y = INT32_MIN;
	return true;
}

bool cursor_plane_show_pixels(struct flame_cursor_plane *plane, const void *key,
		uint64_t seq, const void *pixels, int width, int height, int stride,
		int hotspot_x, int hotspot_y) {
	if (!plane->usable) {
		return false;
	}
	if (plane->shown == key && plane->shown_seq == seq) {
		return true;
	}
	struct flame_dumb_buffer *buffer = next_buffer(plane, width, height);
	if (!buffer) {
		return false;
	}
	for (int y = 0; y < height; y++) {
		memcpy((uint8_t *)buffer->data + (size_t)y * buffer->stride,
			(const uint8_t *)pixels + (size_t)y * stride, (size_t)width * 4);
	}
	return hand_over(plane, buffer, key, seq, hotspot_x, hotspot_y);
}

bool cursor_plane_show_texture(struct flame_cursor_plane *plane, const void *key,
		uint64_t seq, struct wlr_texture *texture, int hotspot_x, int hotspot_y) {
	if (!plane->usable) {
		return false;
	}
	if (plane->shown == key && plane->shown_seq == seq) {
		return true;
	}
	struct flame_dumb_buffer *buffer = next_buffer(plane, (int)texture->width,
		(int)texture->height);
	if (!buffer) {
		return false;
	}
	// straight from the gpu into the mapped buffer, clients' cursor buffers
	// are long released by the time we look
	if (!wlr_texture_read_pixels(texture, &(struct wlr_texture_read_pixels_options){
			.data = buffer->data,
			.format = DRM_FORMAT_ARGB8888,
			.stride = buffer->stride,
		})) {
		return false;
	}
	return hand_over(plane, buffer, key, seq, hotspot_x, hotspot_y);
}

void cursor_plane_hide(struct flame_cursor_plane *plane) {
	if (plane->shown && plane->output->impl->set_cursor) {
		plane->output->impl->set_cursor(plane->output, NULL, 0, 0);
		plane->dirty = true;
	}
	plane->shown = NULL;
}

void cursor_plane_forget(struct flame_cursor_plane *plane) {
	// a plane that refused us gets another go, things change across vt switches
	plane->usable = plane->device.fd >= 0;
	plane->shown = NULL;
	plane->dirty = true;
}

bool cursor_plane_move(struct flame_cursor_plane *plane, int x, int y) {
	if (!plane->shown || (x == plane->x && y == plane->y)) {
		return false;
	}
	plane->x = x;
	plane->y = y;
	plane->dirty = true;
	plane->output->impl->move_cursor(plane->output, x, y);
	return true;
}
