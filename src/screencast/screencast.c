#define _GNU_SOURCE // memfd_create
#include "screencast/screencast.h"

#include <drm_fourcc.h>
#include <errno.h>
#include <math.h>
#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

#include "core/server.h"
#include "desktop/view.h"
#include "render/cursor.h"
#include "render/elements.h"
#include "render/snapshot.h"
#include "util/listener.h"

// a stream drives itself (it's the graph's driver): a frame is drawn into an
// empty buffer it holds, once the gpu's done the graph's woken, and in its
// process event the buffer goes out and the next empty one's taken. formats:
// the gpu buffers the screen itself uses (format and modifiers known to draw
// fine here), the app picks which, or shared memory, drawn on the gpu and
// read back

#define BUFFERS 4
// a share that just started gets a frame every time it asks: pipewire is
// still settling, a window that doesn't change would stay black
#define WARM_UP_MS 1000
// an unchanged picture still goes out this often: a viewer joining late
#define KEEPALIVE_MS 1000
// what scene events don't catch
#define FALLBACK_MS 100

struct cast_buffer {
	struct wlr_buffer *gpu; // gpu buffers: the stream's own, drawn into
	void *map; // shared memory, read back into
	size_t size;
	int fd;
	uint32_t stride;
};

struct cast_stream {
	struct wl_list link; // flame_screencast.streams
	struct flame_screencast *screencast;
	struct cast_target target;
	struct cast_stream_events events;
	void *data;

	struct pw_stream *stream;
	struct spa_hook listener;
	uint32_t node;
	bool told; // ready went out
	bool ended;

	int width, height; // what it shows now, pixels
	int nego_width, nego_height; // what the buffers are
	uint32_t fourcc;
	uint64_t *modifiers; // the screen's, what gpu buffers can be
	size_t modifiers_len;
	bool no_dmabuf; // the gpu couldn't make one the app takes
	bool negotiated, dmabuf;
	uint64_t modifier;
	uint32_t planes;
	uint32_t max_fps;
	bool streaming;

	struct pw_buffer *empty, *filled; // held to draw into, drawn and waiting
	bool drawing; // the gpu's at work on empty
	struct wlr_buffer *scratch; // shared memory frames are drawn here first
	struct wlr_drm_syncobj_timeline *timeline;
	uint64_t point;
	struct wlr_drm_syncobj_timeline_waiter waiter;
	bool waiting;

	uint64_t look, seq;
	int64_t started, sent;
	bool force;
	struct wl_event_source *tick;
	struct wl_listener output_destroy;
	struct wl_event_source *ending; // ended goes out from here, not mid event
};

static int64_t now_ms(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static uint32_t video_format(uint32_t fourcc) {
	switch (fourcc) {
	case DRM_FORMAT_XRGB8888:
		return SPA_VIDEO_FORMAT_BGRx;
	case DRM_FORMAT_ARGB8888:
		return SPA_VIDEO_FORMAT_BGRA;
	default:
		return SPA_VIDEO_FORMAT_UNKNOWN;
	}
}

static struct flame_output *screen_of(struct cast_stream *stream) {
	return stream->target.output ? stream->target.output
		: outputs_primary(&stream->screencast->server->outputs);
}

static float scale_of(struct cast_stream *stream) {
	struct flame_output *output = screen_of(stream);
	return output && output->wlr->scale > 0 ? output->wlr->scale : 1;
}

// what it shows in layout coords (a window: its tree's), false for nothing
static bool area_of(struct cast_stream *stream, struct wlr_box *area) {
	if (stream->target.view) {
		struct flame_view *view = stream->target.view;
		int width = 0, height = 0;
		view->impl->content_size(view, &width, &height);
		*area = (struct wlr_box){view->content->node.x, view->content->node.y, width, height};
		return width > 0 && height > 0;
	}
	*area = stream->target.output->box;
	return !wlr_box_empty(area);
}

static void handle_ending(void *data) {
	struct cast_stream *stream = data;
	stream->ending = NULL;
	if (stream->events.ended) {
		stream->events.ended(stream, stream->data); // may destroy it
	}
}

// it can't go on: its owner hears it once whatever event this came from is
// over, the stream's safe to destroy then
static void end(struct cast_stream *stream) {
	if (stream->ended) {
		return;
	}
	stream->ended = true;
	stream->streaming = false;
	stream->ending = wl_event_loop_add_idle(stream->screencast->server->loop, handle_ending,
		stream);
}

// ---- formats ----

// the screen's own: a format and modifiers its buffers are drawn in
static bool take_formats(struct cast_stream *stream) {
	struct flame_output *output = screen_of(stream);
	struct wlr_swapchain *chain = output ? output->wlr->swapchain : NULL;
	stream->fourcc = chain ? chain->format.format : DRM_FORMAT_XRGB8888;
	if (video_format(stream->fourcc) == SPA_VIDEO_FORMAT_UNKNOWN) {
		stream->fourcc = DRM_FORMAT_XRGB8888;
		chain = NULL; // its modifiers are for another format
	}
	free(stream->modifiers);
	stream->modifiers = NULL;
	stream->modifiers_len = 0;
	if (!chain || !chain->format.len) {
		return true;
	}
	stream->modifiers = calloc(chain->format.len, sizeof(*stream->modifiers));
	if (!stream->modifiers) {
		return false;
	}
	memcpy(stream->modifiers, chain->format.modifiers,
		chain->format.len * sizeof(*stream->modifiers));
	stream->modifiers_len = chain->format.len;
	wlr_log(WLR_DEBUG, "screencast: gpu buffers in %zu layouts", stream->modifiers_len);
	return true;
}

static uint32_t drm_format(uint32_t video) {
	return video == SPA_VIDEO_FORMAT_BGRA ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888;
}

// format: a gpu buffer's (with modifiers), the shared memory one offers both
static const struct spa_pod *format_pod(struct spa_pod_builder *b, struct cast_stream *stream,
		uint32_t format, const uint64_t *modifiers, size_t len, bool fixed) {
	struct spa_pod_frame f[2];
	spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	spa_pod_builder_add(b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
	spa_pod_builder_add(b, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
	if (len) {
		// modifiers go with their format, nothing else to offer with them
		spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format, SPA_POD_Id(format), 0);
		if (fixed) {
			spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
			spa_pod_builder_long(b, (int64_t)modifiers[0]);
		} else {
			spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier,
				SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
			spa_pod_builder_push_choice(b, &f[1], SPA_CHOICE_Enum, 0);
			spa_pod_builder_long(b, (int64_t)modifiers[0]); // the default
			for (size_t i = 0; i < len; i++) {
				spa_pod_builder_long(b, (int64_t)modifiers[i]);
			}
			spa_pod_builder_pop(b, &f[1]);
		}
	} else {
		spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format,
			SPA_POD_CHOICE_ENUM_Id(3, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx,
				SPA_VIDEO_FORMAT_BGRA), 0);
	}
	spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size,
		SPA_POD_Rectangle(&SPA_RECTANGLE((uint32_t)stream->width, (uint32_t)stream->height)), 0);
	// as fast as it changes, up to the screen's rate
	spa_pod_builder_add(b, SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&SPA_FRACTION(0, 1)),
		0);
	uint32_t fps = stream->max_fps ? stream->max_fps : 60;
	spa_pod_builder_add(b, SPA_FORMAT_VIDEO_maxFramerate,
		SPA_POD_CHOICE_RANGE_Fraction(&SPA_FRACTION(fps, 1), &SPA_FRACTION(1, 1),
			&SPA_FRACTION(fps, 1)), 0);
	return spa_pod_builder_pop(b, &f[0]);
}

// what it offers: gpu buffers first (unless they failed), with alpha or
// without, the app picks (browsers want one, obs the other), the same
// layouts for both. then shared memory. fixed: a modifier picked for the
// app, first
static uint32_t offer(struct spa_pod_builder *b, struct cast_stream *stream,
		const uint64_t *fixed, const struct spa_pod **params) {
	uint32_t n = 0;
	if (fixed) {
		params[n++] = format_pod(b, stream, video_format(stream->fourcc), fixed, 1, true);
	}
	if (!stream->no_dmabuf && stream->modifiers_len) {
		params[n++] = format_pod(b, stream, SPA_VIDEO_FORMAT_BGRx, stream->modifiers,
			stream->modifiers_len, false);
		params[n++] = format_pod(b, stream, SPA_VIDEO_FORMAT_BGRA, stream->modifiers,
			stream->modifiers_len, false);
	}
	params[n++] = format_pod(b, stream, SPA_VIDEO_FORMAT_BGRx, NULL, 0, false);
	return n;
}

static void update_formats(struct cast_stream *stream, const uint64_t *fixed) {
	uint8_t buf[8192];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	const struct spa_pod *params[4];
	uint32_t n = offer(&b, stream, fixed, params);
	pw_stream_update_params(stream->stream, params, n);
}

// a gpu buffer of the stream's size in fourcc with one of modifiers, the
// allocator (gbm) picks which
static struct wlr_buffer *allocate(struct cast_stream *stream, const uint64_t *modifiers,
		size_t len) {
	struct wlr_drm_format format = {
		.format = stream->fourcc,
		.len = len,
		.capacity = len,
		.modifiers = (uint64_t *)modifiers,
	};
	return wlr_allocator_create_buffer(stream->screencast->server->render.allocator,
		stream->width, stream->height, &format);
}

// the app's modifiers it can take, ours among them: a buffer made with
// those says which one it is. none: shared memory from now on
static void fixate(struct cast_stream *stream, const struct spa_pod_prop *prop) {
	const struct spa_pod *value = &prop->value;
	uint32_t n = SPA_POD_CHOICE_N_VALUES(value);
	const uint64_t *theirs = SPA_POD_CHOICE_VALUES(value);
	// the first is the choice's default, the rest the list
	uint64_t *both = calloc(n ? n : 1, sizeof(*both));
	size_t len = 0;
	for (uint32_t i = 1; both && i < n; i++) {
		for (size_t j = 0; j < stream->modifiers_len; j++) {
			if (theirs[i] == stream->modifiers[j]) {
				both[len++] = theirs[i];
				break;
			}
		}
	}
	struct wlr_buffer *test = len ? allocate(stream, both, len) : NULL;
	wlr_log(WLR_DEBUG, "screencast: the app takes %u gpu buffer layouts, %zu of them ours",
		n > 0 ? n - 1 : 0, len);
	free(both);
	struct wlr_dmabuf_attributes dmabuf;
	if (!test || !wlr_buffer_get_dmabuf(test, &dmabuf)) {
		if (test) {
			wlr_buffer_drop(test);
		}
		wlr_log(WLR_INFO, "screencast: no gpu buffer the app takes, shared memory instead");
		stream->no_dmabuf = true;
		update_formats(stream, NULL);
		return;
	}
	uint64_t modifier = dmabuf.modifier;
	stream->planes = (uint32_t)dmabuf.n_planes;
	wlr_buffer_drop(test);
	update_formats(stream, &modifier);
}

// ---- buffers ----

static void buffers_param(struct cast_stream *stream) {
	uint8_t buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	const struct spa_pod *params[2];
	uint32_t stride = (uint32_t)stream->width * 4;
	if (stream->dmabuf) {
		params[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamBuffers,
			SPA_PARAM_Buffers,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(BUFFERS, 2, 8),
			SPA_PARAM_BUFFERS_blocks, SPA_POD_Int((int32_t)stream->planes),
			SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_DmaBuf));
	} else {
		params[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamBuffers,
			SPA_PARAM_Buffers,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(BUFFERS, 2, 8),
			SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
			SPA_PARAM_BUFFERS_size, SPA_POD_Int((int32_t)(stride * (uint32_t)stream->height)),
			SPA_PARAM_BUFFERS_stride, SPA_POD_Int((int32_t)stride),
			SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_MemFd));
	}
	params[1] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
		SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
	pw_stream_update_params(stream->stream, params, 2);
}

static void handle_param_changed(void *data, uint32_t id, const struct spa_pod *param) {
	struct cast_stream *stream = data;
	if (id != SPA_PARAM_Format || !param) {
		return;
	}
	struct spa_video_info_raw info;
	if (spa_format_video_raw_parse(param, &info) < 0) {
		return;
	}
	const struct spa_pod_prop *modifier = spa_pod_find_prop(param, NULL,
		SPA_FORMAT_VIDEO_modifier);
	// gpu buffers in what the app picked, alpha or not
	stream->fourcc = drm_format(info.format);
	if (modifier && modifier->flags & SPA_POD_PROP_FLAG_DONT_FIXATE) {
		fixate(stream, modifier);
		return; // the app hears our pick, the format comes again fixed
	}
	stream->dmabuf = modifier != NULL;
	stream->modifier = info.modifier;
	if (stream->dmabuf && !stream->planes) {
		// fixed by the app itself: how many planes that takes
		struct wlr_buffer *test = allocate(stream, &stream->modifier, 1);
		struct wlr_dmabuf_attributes dmabuf;
		stream->planes = test && wlr_buffer_get_dmabuf(test, &dmabuf)
			? (uint32_t)dmabuf.n_planes : 1;
		if (test) {
			wlr_buffer_drop(test);
		}
	}
	stream->nego_width = (int)info.size.width;
	stream->nego_height = (int)info.size.height;
	if (info.max_framerate.denom && info.max_framerate.num) {
		stream->max_fps = info.max_framerate.num / info.max_framerate.denom;
	}
	stream->negotiated = true;
	wlr_log(WLR_DEBUG, "screencast: %dx%d in %s%s, up to %u fps", stream->nego_width,
		stream->nego_height, stream->dmabuf ? "gpu buffers" : "shared memory",
		stream->dmabuf ? "" : " (read back)", stream->max_fps);
	buffers_param(stream);
}

static void buffer_free(struct cast_buffer *cb) {
	if (cb->gpu) {
		wlr_buffer_drop(cb->gpu);
	}
	if (cb->map && cb->map != MAP_FAILED) {
		munmap(cb->map, cb->size);
	}
	if (cb->fd >= 0) {
		close(cb->fd);
	}
	free(cb);
}

static void handle_add_buffer(void *data, struct pw_buffer *pw_buffer) {
	struct cast_stream *stream = data;
	struct spa_data *d = pw_buffer->buffer->datas;
	struct cast_buffer *cb = calloc(1, sizeof(*cb));
	if (!cb) {
		return;
	}
	cb->fd = -1;
	pw_buffer->user_data = cb;
	if (d[0].type & (1u << SPA_DATA_DmaBuf)) {
		cb->gpu = allocate(stream, &stream->modifier, 1);
		struct wlr_dmabuf_attributes dmabuf;
		if (!cb->gpu || !wlr_buffer_get_dmabuf(cb->gpu, &dmabuf)
				|| (uint32_t)dmabuf.n_planes != pw_buffer->buffer->n_datas) {
			wlr_log(WLR_ERROR, "screencast: no gpu buffer for the stream");
			return;
		}
		for (uint32_t i = 0; i < pw_buffer->buffer->n_datas; i++) {
			d[i].type = SPA_DATA_DmaBuf;
			d[i].flags = SPA_DATA_FLAG_READABLE;
			d[i].fd = dmabuf.fd[i];
			d[i].mapoffset = 0;
			d[i].maxsize = dmabuf.stride[i] * (uint32_t)stream->height;
			d[i].data = NULL;
			d[i].chunk->offset = dmabuf.offset[i];
			d[i].chunk->stride = (int32_t)dmabuf.stride[i];
			d[i].chunk->size = d[i].maxsize;
		}
		return;
	}
	cb->stride = (uint32_t)stream->width * 4;
	cb->size = (size_t)cb->stride * (size_t)stream->height;
	cb->fd = memfd_create("flamewm-screencast", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (cb->fd < 0 || ftruncate(cb->fd, (off_t)cb->size) < 0) {
		wlr_log(WLR_ERROR, "screencast: no memory for the stream: %s", strerror(errno));
		return;
	}
	cb->map = mmap(NULL, cb->size, PROT_READ | PROT_WRITE, MAP_SHARED, cb->fd, 0);
	if (cb->map == MAP_FAILED) {
		cb->map = NULL;
		return;
	}
	d[0].type = SPA_DATA_MemFd;
	d[0].flags = SPA_DATA_FLAG_READABLE | SPA_DATA_FLAG_MAPPABLE;
	d[0].fd = cb->fd;
	d[0].mapoffset = 0;
	d[0].maxsize = (uint32_t)cb->size;
	d[0].data = cb->map;
	d[0].chunk->offset = 0;
	d[0].chunk->stride = (int32_t)cb->stride;
	d[0].chunk->size = (uint32_t)cb->size;
}

static void handle_remove_buffer(void *data, struct pw_buffer *pw_buffer) {
	struct cast_stream *stream = data;
	if (stream->empty == pw_buffer) {
		if (stream->waiting) {
			wlr_drm_syncobj_timeline_waiter_finish(&stream->waiter);
			stream->waiting = false;
		}
		stream->drawing = false;
		stream->empty = NULL;
	}
	if (stream->filled == pw_buffer) {
		stream->filled = NULL;
	}
	if (pw_buffer->user_data) {
		buffer_free(pw_buffer->user_data);
		pw_buffer->user_data = NULL;
	}
}

// ---- frames ----

static void arm(struct cast_stream *stream, int ms) {
	wl_event_source_timer_update(stream->tick, ms > 0 ? ms : 1);
}

// the drawn frame goes: the graph's woken, process sends it
static void drawn(struct cast_stream *stream) {
	struct pw_buffer *pw_buffer = stream->empty;
	stream->drawing = false;
	if (!pw_buffer) {
		return;
	}
	struct spa_buffer *buffer = pw_buffer->buffer;
	struct spa_meta_header *header = spa_buffer_find_meta_data(buffer, SPA_META_Header,
		sizeof(*header));
	if (header) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		header->pts = SPA_TIMESPEC_TO_NSEC(&now);
		header->flags = 0;
		header->seq = stream->seq++;
		header->dts_offset = 0;
	}
	for (uint32_t i = 0; i < buffer->n_datas; i++) {
		buffer->datas[i].chunk->flags = SPA_CHUNK_FLAG_NONE;
	}
	stream->filled = pw_buffer;
	stream->empty = NULL;
	pw_stream_trigger_process(stream->stream);
}

static void handle_drawn(struct wlr_drm_syncobj_timeline_waiter *waiter) {
	struct cast_stream *stream = wl_container_of(waiter, stream, waiter);
	wlr_drm_syncobj_timeline_waiter_finish(&stream->waiter);
	stream->waiting = false;
	drawn(stream);
}

// what it shows, collected. a screen gets the pointer on top
static bool collect(struct cast_stream *stream, struct flame_element_list *list,
		struct wlr_box *area) {
	if (!area_of(stream, area)) {
		return false;
	}
	if (stream->target.view) {
		return elements_collect_node(list, &stream->target.view->content->node, *area);
	}
	struct flame_output *output = stream->target.output;
	bool ok = elements_collect(list, &stream->screencast->server->render.scene, *area);
	if (ok && stream->target.cursor) {
		ok = frame_cursor_push(&output->frames.cursor, *area, list);
	}
	return ok;
}

// a frame if it's due and there's a buffer for it: what it shows changed,
// it's warming up, or the keepalive's up
static void produce(struct cast_stream *stream) {
	if (!stream->streaming || !stream->negotiated || stream->drawing || stream->filled
			|| stream->ended) {
		return;
	}
	struct wlr_box area;
	float scale = scale_of(stream);
	struct flame_element_list list;
	elements_init(&list);
	if (!collect(stream, &list, &area)) {
		elements_finish(&list);
		arm(stream, FALLBACK_MS);
		return;
	}
	int width = (int)ceilf(area.width * scale), height = (int)ceilf(area.height * scale);
	if (width != stream->nego_width || height != stream->nego_height) {
		// resized: new buffers first, the format's asked again
		if (width != stream->width || height != stream->height) {
			stream->width = width;
			stream->height = height;
			stream->negotiated = false;
			update_formats(stream, NULL);
		}
		elements_finish(&list);
		return;
	}
	int64_t now = now_ms();
	uint64_t look = elements_look(&list, area, scale);
	bool due = stream->force || now - stream->started < WARM_UP_MS
		|| now - stream->sent >= KEEPALIVE_MS || look != stream->look;
	int gap = stream->max_fps ? (int)(1000 / stream->max_fps) : 0;
	int wait = due ? (int)(stream->sent + gap - now) : FALLBACK_MS;
	if (!stream->empty && due && wait <= 0) {
		stream->empty = pw_stream_dequeue_buffer(stream->stream);
		if (!stream->empty) {
			wait = 4; // the app holds them all, soon
		}
	}
	struct cast_buffer *cb = stream->empty ? stream->empty->user_data : NULL;
	if (due && wait <= 0 && cb) {
		struct flame_render *render = &stream->screencast->server->render;
		if (cb->gpu) {
			uint64_t point = stream->point + 1;
			struct wlr_drm_syncobj_timeline *timeline = stream->timeline;
			if (render_elements_to_buffer(render, &list, area, scale, cb->gpu, timeline,
					point)) {
				stream->point = point;
				stream->drawing = true;
				stream->waiting = timeline && wlr_drm_syncobj_timeline_waiter_init(
					&stream->waiter, timeline, point, 0, stream->screencast->server->loop,
					handle_drawn);
				if (!stream->waiting) {
					drawn(stream); // implicit sync: reads are ordered after
				}
			}
		} else if (cb->map) {
			if (!stream->scratch) {
				stream->scratch = allocate(stream, stream->modifiers, stream->modifiers_len);
			}
			struct wlr_texture *texture = NULL;
			if (stream->scratch && render_elements_to_buffer(render, &list, area, scale,
					stream->scratch, NULL, 0)) {
				texture = wlr_texture_from_buffer(render->renderer, stream->scratch);
			}
			if (texture && wlr_texture_read_pixels(texture,
					&(struct wlr_texture_read_pixels_options){
						.data = cb->map,
						.format = stream->fourcc,
						.stride = cb->stride,
					})) {
				drawn(stream);
			}
			if (texture) {
				wlr_texture_destroy(texture);
			}
		}
		stream->look = look;
		stream->sent = now;
		stream->force = false;
	} else if (due) {
		arm(stream, wait > 0 ? wait : 4);
	} else {
		arm(stream, FALLBACK_MS);
	}
	// its clients draw on for whoever watches, on a hidden workspace too
	struct timespec when;
	clock_gettime(CLOCK_MONOTONIC, &when);
	for (size_t i = 0; i < list.len; i++) {
		if (list.data[i].surface) {
			scene_surface_frame_done(list.data[i].surface, &when);
		}
	}
	elements_finish(&list);
}

static int handle_tick(void *data) {
	produce(data);
	return 0;
}

static void handle_process(void *data) {
	struct cast_stream *stream = data;
	if (stream->filled) {
		pw_stream_queue_buffer(stream->stream, stream->filled);
		stream->filled = NULL;
	}
	// right away when something's waiting to go out
	arm(stream, 1);
}

static void handle_state_changed(void *data, enum pw_stream_state old,
		enum pw_stream_state state, const char *error) {
	struct cast_stream *stream = data;
	wlr_log(WLR_DEBUG, "screencast: stream %s%s%s", pw_stream_state_as_string(state),
		error ? ": " : "", error ? error : "");
	uint32_t node = pw_stream_get_node_id(stream->stream);
	if (node != SPA_ID_INVALID && !stream->told) {
		stream->told = true;
		stream->node = node;
		if (stream->events.ready) {
			stream->events.ready(stream, node, stream->width, stream->height, stream->data);
		}
	}
	switch (state) {
	case PW_STREAM_STATE_STREAMING:
		stream->streaming = true;
		stream->started = now_ms();
		stream->force = true;
		arm(stream, 1);
		break;
	case PW_STREAM_STATE_ERROR:
		stream->streaming = false;
		end(stream);
		break;
	case PW_STREAM_STATE_UNCONNECTED:
		stream->streaming = false;
		if (old != PW_STREAM_STATE_UNCONNECTED) {
			end(stream);
		}
		break;
	default:
		stream->streaming = false;
		break;
	}
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = handle_state_changed,
	.param_changed = handle_param_changed,
	.add_buffer = handle_add_buffer,
	.remove_buffer = handle_remove_buffer,
	.process = handle_process,
};

// ---- pipewire ----

static int handle_loop(int fd, uint32_t mask, void *data) {
	struct flame_screencast *screencast = data;
	pw_loop_iterate(screencast->loop, 0);
	return 0;
}

static void handle_core_error(void *data, uint32_t id, int seq, int res, const char *message) {
	struct flame_screencast *screencast = data;
	wlr_log(WLR_ERROR, "screencast: pipewire: %s", message ? message : spa_strerror(res));
	if (id == PW_ID_CORE && res == -EPIPE) {
		// pipewire went: every stream ends, the next one connects again
		struct cast_stream *stream, *tmp;
		wl_list_for_each_safe(stream, tmp, &screencast->streams, link) {
			end(stream);
		}
		screencast->usable = false;
	}
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = handle_core_error,
};

static struct spa_hook core_listener;

// connected now, or tried: pipewire may start after flame does
static bool connect_pipewire(struct flame_screencast *screencast) {
	if (screencast->usable) {
		return true;
	}
	if (screencast->core) {
		spa_hook_remove(&core_listener);
		pw_core_disconnect(screencast->core);
		screencast->core = NULL;
	}
	screencast->core = pw_context_connect(screencast->context, NULL, 0);
	if (!screencast->core) {
		wlr_log(WLR_ERROR, "screencast: can't reach pipewire: %s", strerror(errno));
		return false;
	}
	spa_zero(core_listener);
	pw_core_add_listener(screencast->core, &core_listener, &core_events, screencast);
	screencast->usable = true;
	return true;
}

static void handle_poke(void *data) {
	struct flame_screencast *screencast = data;
	screencast->poke = NULL;
	struct cast_stream *stream;
	wl_list_for_each(stream, &screencast->streams, link) {
		produce(stream);
	}
}

static void poke(struct flame_screencast *screencast) {
	if (!screencast->poke && !wl_list_empty(&screencast->streams)) {
		screencast->poke = wl_event_loop_add_idle(screencast->server->loop, handle_poke,
			screencast);
	}
}

static void handle_scene_commit(struct wl_listener *listener, void *data) {
	struct flame_screencast *screencast = wl_container_of(listener, screencast, scene_commit);
	poke(screencast);
}

static void handle_scene_changed(struct wl_listener *listener, void *data) {
	struct flame_screencast *screencast = wl_container_of(listener, screencast,
		scene_changed);
	poke(screencast);
}

static void handle_output_destroy(struct wl_listener *listener, void *data) {
	struct cast_stream *stream = wl_container_of(listener, stream, output_destroy);
	listener_disconnect(&stream->output_destroy);
	stream->target.output = NULL;
	end(stream);
}

struct cast_stream *screencast_stream_create(struct flame_screencast *screencast,
		const struct cast_target *target, const struct cast_stream_events *events, void *data) {
	if (!connect_pipewire(screencast)) {
		return NULL;
	}
	struct cast_stream *stream = calloc(1, sizeof(*stream));
	if (!stream) {
		return NULL;
	}
	stream->screencast = screencast;
	stream->target = *target;
	stream->events = *events;
	stream->data = data;
	stream->node = SPA_ID_INVALID;
	wl_list_init(&stream->output_destroy.link);
	struct wlr_box area;
	float scale = scale_of(stream);
	struct flame_output *screen = screen_of(stream);
	stream->max_fps = screen && screen->wlr->refresh > 0
		? (uint32_t)((screen->wlr->refresh + 500) / 1000) : 60;
	if (!area_of(stream, &area) || !take_formats(stream)) {
		free(stream->modifiers);
		free(stream);
		return NULL;
	}
	stream->width = (int)ceilf(area.width * scale);
	stream->height = (int)ceilf(area.height * scale);
	struct wlr_renderer *renderer = screencast->server->render.renderer;
	int drm_fd = wlr_renderer_get_drm_fd(renderer);
	if (renderer->features.timeline && drm_fd >= 0) {
		stream->timeline = wlr_drm_syncobj_timeline_create(drm_fd);
	}
	stream->tick = wl_event_loop_add_timer(screencast->server->loop, handle_tick, stream);
	stream->stream = pw_stream_new(screencast->core, "flamewm",
		pw_properties_new(PW_KEY_MEDIA_CLASS, "Video/Source", NULL));
	if (!stream->tick || !stream->stream) {
		if (stream->tick) {
			wl_event_source_remove(stream->tick);
		}
		if (stream->timeline) {
			wlr_drm_syncobj_timeline_unref(stream->timeline);
		}
		free(stream->modifiers);
		free(stream);
		return NULL;
	}
	if (target->output) {
		listener_connect(&stream->output_destroy, &target->output->wlr->events.destroy,
			handle_output_destroy);
	}
	wl_list_insert(&screencast->streams, &stream->link);
	pw_stream_add_listener(stream->stream, &stream->listener, &stream_events, stream);
	uint8_t buf[8192];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	const struct spa_pod *params[4];
	uint32_t n = offer(&b, stream, NULL, params);
	if (pw_stream_connect(stream->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
			PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_ALLOC_BUFFERS, params, n) < 0) {
		wlr_log(WLR_ERROR, "screencast: pipewire won't take the stream");
		screencast_stream_destroy(stream);
		return NULL;
	}
	wlr_log(WLR_DEBUG, "screencast: a stream of %s, %dx%d", target->view ? "a window"
		: target->output->wlr->name, stream->width, stream->height);
	return stream;
}

void screencast_stream_destroy(struct cast_stream *stream) {
	// disconnecting says unconnected: that's no end to tell anyone about
	stream->ended = true;
	if (stream->ending) {
		wl_event_source_remove(stream->ending);
	}
	if (stream->waiting) {
		wlr_drm_syncobj_timeline_waiter_finish(&stream->waiter);
	}
	// buffers go through remove_buffer as it disconnects
	pw_stream_disconnect(stream->stream);
	pw_stream_destroy(stream->stream);
	listener_disconnect(&stream->output_destroy);
	wl_event_source_remove(stream->tick);
	if (stream->scratch) {
		wlr_buffer_drop(stream->scratch);
	}
	if (stream->timeline) {
		wlr_drm_syncobj_timeline_unref(stream->timeline);
	}
	wl_list_remove(&stream->link);
	free(stream->modifiers);
	free(stream);
}

void screencast_view_gone(struct flame_screencast *screencast, struct flame_view *view) {
	struct cast_stream *stream, *tmp;
	wl_list_for_each_safe(stream, tmp, &screencast->streams, link) {
		if (stream->target.view == view) {
			end(stream);
		}
	}
}

bool screencast_init(struct flame_screencast *screencast, struct flame_server *server) {
	screencast->server = server;
	wl_list_init(&screencast->streams);
	pw_init(NULL, NULL);
	screencast->loop = pw_loop_new(NULL);
	screencast->context = screencast->loop ? pw_context_new(screencast->loop, NULL, 0) : NULL;
	if (!screencast->context) {
		wlr_log(WLR_ERROR, "screencast: no pipewire loop");
		return false;
	}
	pw_loop_enter(screencast->loop);
	screencast->loop_source = wl_event_loop_add_fd(server->loop,
		pw_loop_get_fd(screencast->loop), WL_EVENT_READABLE, handle_loop, screencast);
	listener_connect(&screencast->scene_commit, &server->render.scene.events.commit,
		handle_scene_commit);
	listener_connect(&screencast->scene_changed, &server->render.scene.events.changed,
		handle_scene_changed);
	return screencast->loop_source != NULL;
}

void screencast_finish(struct flame_screencast *screencast) {
	struct cast_stream *stream, *tmp;
	wl_list_for_each_safe(stream, tmp, &screencast->streams, link) {
		screencast_stream_destroy(stream);
	}
	listener_disconnect(&screencast->scene_commit);
	listener_disconnect(&screencast->scene_changed);
	if (screencast->poke) {
		wl_event_source_remove(screencast->poke);
		screencast->poke = NULL;
	}
	if (screencast->loop_source) {
		wl_event_source_remove(screencast->loop_source);
		screencast->loop_source = NULL;
	}
	if (screencast->core) {
		spa_hook_remove(&core_listener);
		pw_core_disconnect(screencast->core);
		screencast->core = NULL;
	}
	if (screencast->context) {
		pw_context_destroy(screencast->context);
		screencast->context = NULL;
	}
	if (screencast->loop) {
		pw_loop_leave(screencast->loop);
		pw_loop_destroy(screencast->loop);
		screencast->loop = NULL;
	}
}
