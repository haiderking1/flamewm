#include "capture/capture.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wlr/interfaces/wlr_ext_image_capture_source_v1.h>
#include <wlr/render/allocator.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

#include "core/server.h"
#include "desktop/view.h"
#include "render/elements.h"
#include "render/snapshot.h"
#include "scene/scene.h"
#include "util/listener.h"

// a window's capture source: it draws the window's content node on its own
// into a buffer of the window's size at the screen's scale. a frame goes out
// when a client asks for one and the window looks different from the last
// one (a surface committed, something moved), checked at the screen's
// refresh only while someone waits

struct window_source;

struct capture_window {
	struct wl_list link; // flame_capture.windows
	struct flame_capture *capture;
	struct flame_view *view;
	struct wlr_ext_foreign_toplevel_handle_v1 *handle;
	char *title, *app_id; // what the handle says, to tell a change
	struct window_source *source; // NULL till someone captures it
	bool seen; // still mapped at this sync
};

struct window_source {
	struct wlr_ext_image_capture_source_v1 base;
	struct flame_capture *capture;
	struct capture_window *window; // NULL once it's gone
	int started;
	bool wanted; // a client waits for a frame
	bool force; // and it wants one whatever changed (its first)
	uint64_t look; // what the last frame showed
	struct wlr_swapchain *swapchain;
	struct wl_event_source *tick;
};

struct source_frame {
	struct wlr_ext_image_capture_source_v1_frame_event base;
	struct wlr_buffer *buffer;
	struct timespec when;
};

static struct flame_output *screen(struct flame_capture *capture) {
	return outputs_primary(&capture->server->outputs);
}

static float window_scale(struct flame_capture *capture) {
	struct flame_output *output = screen(capture);
	return output && output->wlr->scale > 0 ? output->wlr->scale : 1;
}

// the window as the user sees it, no border, in its tree's coords
static bool window_area(struct flame_view *view, struct wlr_box *area) {
	int width = 0, height = 0;
	view->impl->content_size(view, &width, &height);
	*area = (struct wlr_box){view->content->node.x, view->content->node.y, width, height};
	return width > 0 && height > 0;
}

static uint64_t mix(uint64_t hash, uint64_t value) {
	return (hash ^ value) * 0x100000001b3ull;
}

static uint64_t float_bits(float value) {
	uint32_t bits;
	memcpy(&bits, &value, sizeof(bits));
	return bits;
}

// everything that changes what the window looks like: what's in it, where,
// which buffer, a commit into the same buffer
static uint64_t look_of(const struct flame_element_list *list, struct wlr_box area,
		float scale) {
	uint64_t hash = 0xcbf29ce484222325ull;
	hash = mix(hash, (uint64_t)area.width << 32 | (uint32_t)area.height);
	hash = mix(hash, float_bits(scale));
	for (size_t i = 0; i < list->len; i++) {
		const struct flame_element *e = &list->data[i];
		hash = mix(hash, (uint64_t)e->type);
		hash = mix(hash, (uintptr_t)e->id);
		hash = mix(hash, (uint64_t)(uint32_t)e->box.x << 32 | (uint32_t)e->box.y);
		hash = mix(hash, (uint64_t)(uint32_t)e->box.width << 32 | (uint32_t)e->box.height);
		hash = mix(hash, (uint64_t)(uint32_t)e->clip.width << 32 | (uint32_t)e->clip.height);
		hash = mix(hash, (uintptr_t)e->buffer);
		hash = mix(hash, (uintptr_t)e->image);
		hash = mix(hash, (uintptr_t)e->texture);
		hash = mix(hash, e->surface ? e->surface->current.seq : 0);
		hash = mix(hash, float_bits(e->alpha));
		for (int c = 0; c < 4; c++) {
			hash = mix(hash, float_bits(e->color[c]));
		}
	}
	return hash;
}

// ms between looks at the window: the screen's refresh
static int period(struct flame_capture *capture) {
	struct flame_output *output = screen(capture);
	int refresh = output && output->wlr->refresh > 0 ? output->wlr->refresh : 60000;
	int ms = 1000000 / refresh;
	return ms < 4 ? 4 : ms;
}

// buffers of the window's size, in the format the screen's frames use (known
// to work with this gpu). a new size tells the clients to reallocate
static bool source_size(struct window_source *source, int width, int height) {
	struct wlr_swapchain *chain = source->swapchain;
	if (chain && chain->width == width && chain->height == height) {
		return true;
	}
	struct flame_output *output = screen(source->capture);
	struct flame_render *render = &source->capture->server->render;
	if (!output || !output->wlr->swapchain) {
		return false;
	}
	struct wlr_swapchain *made = wlr_swapchain_create(render->allocator, width, height,
		&output->wlr->swapchain->format);
	if (!made) {
		return false;
	}
	if (chain) {
		wlr_swapchain_destroy(chain);
	}
	source->swapchain = made;
	wlr_ext_image_capture_source_v1_set_constraints_from_swapchain(&source->base, made,
		render->renderer);
	return true;
}

static void source_produce(struct window_source *source) {
	struct capture_window *window = source->window;
	if (!window || !source->started || !source->wanted) {
		return;
	}
	struct flame_capture *capture = source->capture;
	struct flame_view *view = window->view;
	float scale = window_scale(capture);
	struct wlr_box area;
	struct flame_element_list list;
	elements_init(&list);
	bool ok = window_area(view, &area) && elements_collect_node(&list, &view->content->node,
		area);
	uint64_t look = ok ? look_of(&list, area, scale) : 0;
	struct wlr_buffer *buffer = NULL;
	if (ok && (source->force || look != source->look)
			&& source_size(source, (int)ceilf(area.width * scale),
				(int)ceilf(area.height * scale))) {
		buffer = wlr_swapchain_acquire(source->swapchain);
	}
	if (buffer && render_elements_to_buffer(&capture->server->render, &list, area, scale,
			buffer)) {
		source->look = look;
		source->wanted = false;
		source->force = false;
		struct source_frame frame = {.buffer = buffer};
		clock_gettime(CLOCK_MONOTONIC, &frame.when);
		pixman_region32_t damage;
		pixman_region32_init_rect(&damage, 0, 0, (unsigned)buffer->width,
			(unsigned)buffer->height);
		frame.base.damage = &damage;
		wl_signal_emit_mutable(&source->base.events.frame, &frame.base);
		pixman_region32_fini(&damage);
	}
	if (buffer) {
		wlr_buffer_unlock(buffer);
	}
	// its clients draw on for whoever watches, on a hidden workspace too
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	for (size_t i = 0; i < list.len; i++) {
		if (list.data[i].surface) {
			wlr_surface_send_frame_done(list.data[i].surface, &now);
		}
	}
	elements_finish(&list);
	if (source->wanted) {
		wl_event_source_timer_update(source->tick, period(capture));
	}
}

static int handle_tick(void *data) {
	source_produce(data);
	return 0;
}

static void source_start(struct wlr_ext_image_capture_source_v1 *base, bool with_cursors) {
	struct window_source *source = wl_container_of(base, source, base);
	if (source->started++ == 0) {
		source->look = 0;
	}
}

static void source_stop(struct wlr_ext_image_capture_source_v1 *base) {
	struct window_source *source = wl_container_of(base, source, base);
	if (source->started > 0 && --source->started == 0) {
		source->wanted = false;
		wl_event_source_timer_update(source->tick, 0);
	}
}

static void source_request_frame(struct wlr_ext_image_capture_source_v1 *base,
		bool schedule_frame) {
	struct window_source *source = wl_container_of(base, source, base);
	source->wanted = true;
	source->force |= schedule_frame;
	// not from inside the request: right after it
	wl_event_source_timer_update(source->tick, schedule_frame ? 1 : period(source->capture));
}

static void source_copy_frame(struct wlr_ext_image_capture_source_v1 *base,
		struct wlr_ext_image_copy_capture_frame_v1 *dst,
		struct wlr_ext_image_capture_source_v1_frame_event *base_event) {
	struct window_source *source = wl_container_of(base, source, base);
	struct source_frame *frame = wl_container_of(base_event, frame, base);
	if (wlr_ext_image_copy_capture_frame_v1_copy_buffer(dst, frame->buffer,
			source->capture->server->render.renderer)) {
		wlr_ext_image_copy_capture_frame_v1_ready(dst, WL_OUTPUT_TRANSFORM_NORMAL, &frame->when);
	}
}

static const struct wlr_ext_image_capture_source_v1_interface source_impl = {
	.start = source_start,
	.stop = source_stop,
	.request_frame = source_request_frame,
	.copy_frame = source_copy_frame,
};

static struct window_source *source_create(struct capture_window *window) {
	struct flame_capture *capture = window->capture;
	struct window_source *source = calloc(1, sizeof(*source));
	if (!source) {
		return NULL;
	}
	source->capture = capture;
	source->window = window;
	source->tick = wl_event_loop_add_timer(capture->server->loop, handle_tick, source);
	if (!source->tick) {
		free(source);
		return NULL;
	}
	wlr_ext_image_capture_source_v1_init(&source->base, &source_impl);
	// clients learn the size and formats as they start a session
	struct wlr_box area;
	float scale = window_scale(capture);
	if (window_area(window->view, &area)) {
		source_size(source, (int)ceilf(area.width * scale), (int)ceilf(area.height * scale));
	}
	return source;
}

// the window's gone: sessions on it stop, their resources go inert
static void source_destroy(struct window_source *source) {
	wlr_ext_image_capture_source_v1_finish(&source->base);
	wl_event_source_remove(source->tick);
	if (source->swapchain) {
		wlr_swapchain_destroy(source->swapchain);
	}
	free(source);
}

// ---- the toplevel list ----

static void window_destroy(struct capture_window *window) {
	if (window->source) {
		window->source->window = NULL;
		source_destroy(window->source);
	}
	wlr_ext_foreign_toplevel_handle_v1_destroy(window->handle);
	wl_list_remove(&window->link);
	free(window->title);
	free(window->app_id);
	free(window);
}

static bool same(const char *a, const char *b) {
	return a == b || (a && b && !strcmp(a, b));
}

static void window_sync(struct flame_capture *capture, struct flame_view *view) {
	const char *title = view_title(view), *app_id = view_app_id(view);
	struct capture_window *window;
	wl_list_for_each(window, &capture->windows, link) {
		if (window->view != view) {
			continue;
		}
		window->seen = true;
		if (!same(window->title, title) || !same(window->app_id, app_id)) {
			free(window->title);
			free(window->app_id);
			window->title = title ? strdup(title) : NULL;
			window->app_id = app_id ? strdup(app_id) : NULL;
			wlr_ext_foreign_toplevel_handle_v1_update_state(window->handle,
				&(struct wlr_ext_foreign_toplevel_handle_v1_state){title, app_id});
		}
		return;
	}
	window = calloc(1, sizeof(*window));
	if (!window) {
		return;
	}
	window->capture = capture;
	window->view = view;
	window->seen = true;
	window->title = title ? strdup(title) : NULL;
	window->app_id = app_id ? strdup(app_id) : NULL;
	window->handle = wlr_ext_foreign_toplevel_handle_v1_create(capture->list,
		&(struct wlr_ext_foreign_toplevel_handle_v1_state){title, app_id});
	if (!window->handle) {
		free(window->title);
		free(window->app_id);
		free(window);
		return;
	}
	window->handle->data = window;
	wl_list_insert(capture->windows.prev, &window->link);
}

// every mapped window has a handle, an unmapped one doesn't
static void handle_desktop_changed(struct wl_listener *listener, void *data) {
	struct flame_capture *capture = wl_container_of(listener, capture, desktop_changed);
	struct flame_desktop *desktop = &capture->server->desktop;
	struct capture_window *window, *tmp;
	wl_list_for_each(window, &capture->windows, link) {
		window->seen = false;
	}
	for (int i = 0; i < FLAME_WORKSPACE_COUNT; i++) {
		struct flame_workspace *workspace = &desktop->workspaces[i];
		struct flame_view *view;
		wl_list_for_each(view, &workspace->views, link) {
			window_sync(capture, view);
		}
		wl_list_for_each(view, &workspace->floats, link) {
			window_sync(capture, view);
		}
	}
	wl_list_for_each_safe(window, tmp, &capture->windows, link) {
		if (!window->seen) {
			window_destroy(window);
		}
	}
}

static void handle_new_request(struct wl_listener *listener, void *data) {
	struct flame_capture *capture = wl_container_of(listener, capture, new_request);
	struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request *request = data;
	struct capture_window *window = request->toplevel_handle->data;
	if (window && !window->source) {
		window->source = source_create(window);
	}
	// gone or no memory: an inert source, the client hears it stopped
	wlr_ext_foreign_toplevel_image_capture_source_manager_v1_request_accept(request,
		window && window->source ? &window->source->base : NULL);
}

void capture_view_gone(struct flame_capture *capture, struct flame_view *view) {
	struct capture_window *window, *tmp;
	wl_list_for_each_safe(window, tmp, &capture->windows, link) {
		if (window->view == view) {
			window_destroy(window);
		}
	}
}

bool capture_init(struct flame_capture *capture, struct flame_server *server) {
	capture->server = server;
	wl_list_init(&capture->windows);
	capture->list = wlr_ext_foreign_toplevel_list_v1_create(server->display, 1);
	capture->sources = wlr_ext_foreign_toplevel_image_capture_source_manager_v1_create(
		server->display, 1);
	if (!capture->list || !capture->sources) {
		wlr_log(WLR_ERROR, "failed to set up window capture");
		return false;
	}
	listener_connect(&capture->desktop_changed, &server->desktop.events.changed,
		handle_desktop_changed);
	listener_connect(&capture->new_request, &capture->sources->events.new_request,
		handle_new_request);
	return true;
}

void capture_finish(struct flame_capture *capture) {
	struct capture_window *window, *tmp;
	wl_list_for_each_safe(window, tmp, &capture->windows, link) {
		window_destroy(window);
	}
	listener_disconnect(&capture->desktop_changed);
	listener_disconnect(&capture->new_request);
}
