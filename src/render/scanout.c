#include "render/scanout.h"

#include <wlr/backend.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/util/log.h>
#include <wlr/util/transform.h>

#include "render/coords.h"
#include "render/elements.h"
#include "render/render.h"
#include "util/listener.h"

// frames a candidate has to hold (or be gone) before the hint follows
#define DEBOUNCE_FRAMES 30

static void handle_hinted_destroy(struct wl_listener *listener, void *data) {
	struct flame_scanout *scanout = wl_container_of(listener, scanout, hinted_destroy);
	listener_disconnect(&scanout->hinted_destroy);
	scanout->hinted = NULL;
}

void scanout_init(struct flame_scanout *scanout, struct flame_render *render,
		struct wlr_output *output) {
	scanout->render = render;
	scanout->output = output;
	scanout->active = false;
	scanout->suspended = false;
	scanout->debounce = 0;
	scanout->hinted = NULL;
	wl_list_init(&scanout->hinted_destroy.link);
}

// back to the renderer's own formats, or KMS's for the primary plane
static void hint(struct flame_scanout *scanout, struct wlr_surface *surface, bool for_scanout) {
	struct wlr_linux_dmabuf_v1 *dmabuf = scanout->render->dmabuf;
	if (!dmabuf) {
		return;
	}
	if (!for_scanout) {
		wlr_linux_dmabuf_v1_set_surface_feedback(dmabuf, surface, NULL);
		return;
	}
	struct wlr_linux_dmabuf_feedback_v1 feedback = {0};
	if (!wlr_linux_dmabuf_feedback_v1_init_with_options(&feedback,
			&(struct wlr_linux_dmabuf_feedback_v1_init_options){
				.main_renderer = scanout->render->renderer,
				.scanout_primary_output = scanout->output,
			})) {
		return;
	}
	wlr_linux_dmabuf_v1_set_surface_feedback(dmabuf, surface, &feedback);
	wlr_linux_dmabuf_feedback_v1_finish(&feedback);
}

static void unhint(struct flame_scanout *scanout) {
	if (scanout->hinted) {
		hint(scanout, scanout->hinted, false);
		listener_disconnect(&scanout->hinted_destroy);
		scanout->hinted = NULL;
	}
}

void scanout_finish(struct flame_scanout *scanout) {
	unhint(scanout);
}

const struct flame_element *scanout_candidate(const struct flame_element_list *list,
		struct wlr_box area) {
	const struct flame_element *only = NULL;
	for (size_t i = 0; i < list->len; i++) {
		const struct flame_element *element = &list->data[i];
		if (!pixman_region32_not_empty(&element->visible)) {
			continue;
		}
		if (only) {
			return NULL; // two things show, they need composing
		}
		only = element;
	}
	if (!only || only->effected || only->shape_radius > 0) {
		return NULL; // a faded, scaled or rounded one needs drawing
	}
	// a client surface, or a picture whose owner lets KMS show its buffer (a
	// fullscreen X game)
	bool scannable = only->type == FLAME_ELEMENT_SURFACE
		|| (only->type == FLAME_ELEMENT_TEXTURE && only->buffer_node && only->buffer);
	if (!scannable) {
		return NULL;
	}
	pixman_box32_t box = {area.x, area.y, area.x + area.width, area.y + area.height};
	// anything it doesn't cover would have to be painted black by us
	if (pixman_region32_contains_rectangle((pixman_region32_t *)&only->visible, &box)
			!= PIXMAN_REGION_IN) {
		return NULL;
	}
	return only;
}

void scanout_hint(struct flame_scanout *scanout, const struct flame_element *candidate) {
	// only wayland clients get told what KMS takes, X apps allocate on their own
	if (candidate && !candidate->surface) {
		candidate = NULL;
	}
	if (candidate) {
		if (scanout->debounce < DEBOUNCE_FRAMES) {
			scanout->debounce++;
		}
		if (scanout->debounce == DEBOUNCE_FRAMES && scanout->hinted != candidate->surface) {
			unhint(scanout);
			scanout->hinted = candidate->surface;
			listener_connect(&scanout->hinted_destroy, &candidate->surface->events.destroy,
				handle_hinted_destroy);
			hint(scanout, candidate->surface, true);
		}
		return;
	}
	if (scanout->debounce > 0) {
		scanout->debounce--;
	}
	if (scanout->debounce == 0) {
		unhint(scanout);
	}
}

bool scanout_try(struct flame_scanout *scanout, const struct flame_element *candidate,
		const struct flame_render_target *target, struct wlr_output_state *state,
		struct wlr_drm_syncobj_timeline *out_timeline, uint64_t *out_point) {
	struct wlr_output *output = scanout->output;
	struct wlr_buffer *buffer = candidate->buffer;
	// the software cursor (or anything locking rendering) needs composition,
	// and a rotated buffer would need us to turn it
	if (!buffer || scanout->suspended || !wlr_output_is_direct_scanout_allowed(output)
			|| candidate->transform != output->transform) {
		return false;
	}

	struct wlr_output_state pending;
	wlr_output_state_init(&pending);
	if (!wlr_output_state_copy(&pending, state)) {
		return false;
	}

	// the part of the buffer the client shows, where it lands. KMS scales
	// or crops on its own if it can, the test says whether it can
	int width = buffer->width, height = buffer->height;
	wlr_output_transform_coords(candidate->transform, &width, &height);
	struct wlr_fbox whole = {.width = width, .height = height};
	if (!wlr_fbox_empty(&candidate->src) && !wlr_fbox_equal(&candidate->src, &whole)) {
		pending.buffer_src_box = candidate->src;
	}
	struct wlr_box dst = candidate->box;
	target_box_to_buffer(target, &dst);
	pending.buffer_dst_box = dst;

	// the client's own buffer rather than our wrapper around it
	struct wlr_client_buffer *client = candidate->surface ? wlr_client_buffer_get(buffer) : NULL;
	if (client && client->source && client->source->n_locks > 0) {
		buffer = client->source;
	}
	wlr_output_state_set_buffer(&pending, buffer);

	// KMS waits for the client's rendering, then says when it let go
	struct wlr_linux_drm_syncobj_surface_v1_state *sync = candidate->surface
		? wlr_linux_drm_syncobj_v1_get_surface_state(candidate->surface) : NULL;
	if (sync && sync->acquire_timeline) {
		wlr_output_state_set_wait_timeline(&pending, sync->acquire_timeline, sync->acquire_point);
	} else if (!candidate->surface && candidate->wait_timeline) {
		// a picture still being made (an X copy in flight): KMS waits for it if
		// it can, else it's drawn this time. never shown before it's done
		bool done = false;
		if (!wlr_drm_syncobj_timeline_check(candidate->wait_timeline, candidate->wait_point, 0,
				&done)) {
			done = false;
		}
		if (!done && !output->backend->features.timeline) {
			wlr_output_state_finish(&pending);
			return false;
		}
		if (!done) {
			wlr_output_state_set_wait_timeline(&pending, candidate->wait_timeline,
				candidate->wait_point);
		}
	}
	uint64_t point = *out_point + 1;
	if (out_timeline) {
		wlr_output_state_set_signal_timeline(&pending, out_timeline, point);
	}

	if (!wlr_output_test_state(output, &pending)) {
		wlr_output_state_finish(&pending);
		return false;
	}
	wlr_output_state_copy(state, &pending);
	wlr_output_state_finish(&pending);

	*out_point = point;
	if (candidate->surface) {
		wlr_presentation_surface_scanned_out_on_output(candidate->surface, output);
	}
	if (sync && out_timeline) {
		wlr_linux_drm_syncobj_v1_state_add_release_point(sync, out_timeline, point,
			output->event_loop);
	}
	return true;
}
