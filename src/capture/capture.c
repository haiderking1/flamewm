#include "capture/capture.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wlr/interfaces/wlr_ext_image_capture_source_v1.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/render/pass.h>
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
	int started_count; // sessions running
	bool wanted; // a client waits for a frame
	bool force; // and it wants one whatever changed (its first)
	uint64_t look; // what the last frame showed
	int64_t started, sent; // ms: when it started, the last frame
	struct wlr_swapchain *swapchain;
	struct wl_event_source *tick;
	// copies into clients' gpu buffers signal here when the gpu's done
	struct wlr_drm_syncobj_timeline *timeline;
	uint64_t point;
	struct wl_list copies; // pending_copy.link
};

// a frame copied into a client's gpu buffer, told ready once the gpu wrote
// it all: nvidia has no implicit sync, a reader right away would see it half
// written (a tearing stream)
struct pending_copy {
	struct wl_list link; // window_source.copies
	struct wlr_ext_image_copy_capture_frame_v1 *frame;
	struct timespec when;
	struct wlr_drm_syncobj_timeline_waiter waiter;
	struct wl_listener frame_destroy;
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

// a share that just started gets every frame it asks for for this long: the
// first ones go while the stream's still being set up (pipewire renegotiates)
// and a window that doesn't change would leave it black till it did
#define WARM_UP_MS 1000
// an unchanged window still sends a frame this often: someone joining a
// stream late gets a picture
#define KEEPALIVE_MS 1000

static int64_t now_ms(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

// commits and scene changes say when to look again. this is for what they
// don't catch, and the keepalive
#define FALLBACK_MS 100

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
	if (!window || !source->started_count || !source->wanted) {
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
	uint64_t look = ok ? elements_look(&list, area, scale) : 0;
	int64_t now = now_ms();
	bool due = now - source->started < WARM_UP_MS || now - source->sent >= KEEPALIVE_MS;
	struct wlr_buffer *buffer = NULL;
	if (ok && (source->force || due || look != source->look)
			&& source_size(source, (int)ceilf(area.width * scale),
				(int)ceilf(area.height * scale))) {
		buffer = wlr_swapchain_acquire(source->swapchain);
	}
	if (buffer && render_elements_to_buffer(&capture->server->render, &list, area, scale,
			buffer)) {
		source->look = look;
		source->sent = now;
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
	struct timespec when;
	clock_gettime(CLOCK_MONOTONIC, &when);
	for (size_t i = 0; i < list.len; i++) {
		if (list.data[i].surface) {
			wlr_surface_send_frame_done(list.data[i].surface, &when);
		}
	}
	elements_finish(&list);
	if (source->wanted) {
		wl_event_source_timer_update(source->tick, FALLBACK_MS);
	}
}

static int handle_tick(void *data) {
	source_produce(data);
	return 0;
}

// a surface committed or the scene changed: every window someone waits on
// gets a look now, in step with what it draws instead of a timer's beat.
// a look at an unchanged window costs no drawing
static void handle_poke(void *data) {
	struct flame_capture *capture = data;
	capture->poke = NULL;
	struct capture_window *window;
	wl_list_for_each(window, &capture->windows, link) {
		if (window->source && window->source->wanted) {
			source_produce(window->source);
		}
	}
}

static void poke(struct flame_capture *capture) {
	if (capture->poke) {
		return;
	}
	struct capture_window *window;
	wl_list_for_each(window, &capture->windows, link) {
		if (window->source && window->source->wanted) {
			capture->poke = wl_event_loop_add_idle(capture->server->loop, handle_poke, capture);
			return;
		}
	}
}

static void handle_scene_commit(struct wl_listener *listener, void *data) {
	struct flame_capture *capture = wl_container_of(listener, capture, scene_commit);
	poke(capture);
}

static void handle_scene_changed(struct wl_listener *listener, void *data) {
	struct flame_capture *capture = wl_container_of(listener, capture, scene_changed);
	poke(capture);
}

static void source_start(struct wlr_ext_image_capture_source_v1 *base, bool with_cursors) {
	struct window_source *source = wl_container_of(base, source, base);
	if (source->started_count++ == 0) {
		source->look = 0;
		source->started = now_ms();
	}
}

static void source_stop(struct wlr_ext_image_capture_source_v1 *base) {
	struct window_source *source = wl_container_of(base, source, base);
	if (source->started_count > 0 && --source->started_count == 0) {
		source->wanted = false;
		wl_event_source_timer_update(source->tick, 0);
	}
}

static void source_request_frame(struct wlr_ext_image_capture_source_v1 *base,
		bool schedule_frame) {
	struct window_source *source = wl_container_of(base, source, base);
	source->wanted = true;
	source->force |= schedule_frame;
	// not from inside the request: right after it when it wants one now or
	// the share just started, else when the window next draws
	bool warming = now_ms() - source->started < WARM_UP_MS;
	wl_event_source_timer_update(source->tick, schedule_frame || warming ? 1 : FALLBACK_MS);
}

static void copy_free(struct pending_copy *copy) {
	wlr_drm_syncobj_timeline_waiter_finish(&copy->waiter);
	listener_disconnect(&copy->frame_destroy);
	wl_list_remove(&copy->link);
	free(copy);
}

static void handle_copied(struct wlr_drm_syncobj_timeline_waiter *waiter) {
	struct pending_copy *copy = wl_container_of(waiter, copy, waiter);
	// let go first: ready destroys the frame, its destroy would free this
	// copy a second time
	struct wlr_ext_image_copy_capture_frame_v1 *frame = copy->frame;
	struct timespec when = copy->when;
	copy_free(copy);
	wlr_ext_image_copy_capture_frame_v1_ready(frame, WL_OUTPUT_TRANSFORM_NORMAL, &when);
}

static void handle_copy_frame_destroy(struct wl_listener *listener, void *data) {
	struct pending_copy *copy = wl_container_of(listener, copy, frame_destroy);
	copy_free(copy); // the client gave up on it
}

// the frame into dst's gpu buffer, ready once the gpu signals it's done.
// false: it failed, dst heard
static bool copy_synced(struct window_source *source,
		struct wlr_ext_image_copy_capture_frame_v1 *dst, const struct source_frame *frame) {
	struct wlr_renderer *renderer = source->capture->server->render.renderer;
	if (dst->buffer->width != frame->buffer->width
			|| dst->buffer->height != frame->buffer->height) {
		wlr_ext_image_copy_capture_frame_v1_fail(dst,
			EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS);
		return false;
	}
	struct pending_copy *copy = calloc(1, sizeof(*copy));
	struct wlr_texture *texture = copy ? wlr_texture_from_buffer(renderer, frame->buffer) : NULL;
	uint64_t point = source->point + 1;
	struct wlr_render_pass *pass = texture ? wlr_renderer_begin_buffer_pass(renderer,
		dst->buffer, &(struct wlr_buffer_pass_options){
			.signal_timeline = source->timeline,
			.signal_point = point,
		}) : NULL;
	bool ok = false;
	if (pass) {
		wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
			.texture = texture,
			.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
		});
		ok = wlr_render_pass_submit(pass);
	}
	if (texture) {
		wlr_texture_destroy(texture);
	}
	if (ok) {
		source->point = point;
		ok = wlr_drm_syncobj_timeline_waiter_init(&copy->waiter, source->timeline, point, 0,
			source->capture->server->loop, handle_copied);
	}
	if (!ok) {
		free(copy);
		wlr_ext_image_copy_capture_frame_v1_fail(dst,
			EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN);
		return false;
	}
	copy->frame = dst;
	copy->when = frame->when;
	listener_connect(&copy->frame_destroy, &dst->events.destroy, handle_copy_frame_destroy);
	wl_list_insert(&source->copies, &copy->link);
	return true;
}

static void source_copy_frame(struct wlr_ext_image_capture_source_v1 *base,
		struct wlr_ext_image_copy_capture_frame_v1 *dst,
		struct wlr_ext_image_capture_source_v1_frame_event *base_event) {
	struct window_source *source = wl_container_of(base, source, base);
	struct source_frame *frame = wl_container_of(base_event, frame, base);
	struct wlr_dmabuf_attributes dmabuf;
	if (source->timeline && wlr_buffer_get_dmabuf(dst->buffer, &dmabuf)) {
		copy_synced(source, dst, frame);
		return;
	}
	// shared memory: read back right here, done when it returns
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
	wl_list_init(&source->copies);
	struct wlr_renderer *renderer = capture->server->render.renderer;
	int drm_fd = wlr_renderer_get_drm_fd(renderer);
	if (renderer->features.timeline && drm_fd >= 0) {
		source->timeline = wlr_drm_syncobj_timeline_create(drm_fd);
	}
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
	struct pending_copy *copy, *tmp;
	wl_list_for_each_safe(copy, tmp, &source->copies, link) {
		copy_free(copy);
	}
	if (source->timeline) {
		wlr_drm_syncobj_timeline_unref(source->timeline);
	}
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

const char *capture_identifier(struct flame_capture *capture, struct flame_view *view) {
	struct capture_window *window;
	wl_list_for_each(window, &capture->windows, link) {
		if (window->view == view) {
			return window->handle->identifier;
		}
	}
	return NULL;
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
	listener_connect(&capture->scene_commit, &server->render.scene.events.commit,
		handle_scene_commit);
	listener_connect(&capture->scene_changed, &server->render.scene.events.changed,
		handle_scene_changed);
	return true;
}

void capture_finish(struct flame_capture *capture) {
	struct capture_window *window, *tmp;
	wl_list_for_each_safe(window, tmp, &capture->windows, link) {
		window_destroy(window);
	}
	listener_disconnect(&capture->desktop_changed);
	listener_disconnect(&capture->new_request);
	listener_disconnect(&capture->scene_commit);
	listener_disconnect(&capture->scene_changed);
	if (capture->poke) {
		wl_event_source_remove(capture->poke);
		capture->poke = NULL;
	}
}
