// flame's screen sharing portal, called the way xdg-desktop-portal calls it:
// CreateSession, SelectSources, Start (flame's picker asks, the test picks
// with flamectl), then the stream's frames are taken from pipewire like an
// app takes them. with "again", a second share from the first one's restore
// data: no picker, straight to a stream. with "gpu", frames are taken as gpu
// buffers (dmabuf) the way browsers and obs take them, imported with EGL and
// a pixel read back on the gpu.
// with "closed", the share isn't watched: it waits for flame to say it's
// over (its window closed).
// usage: portalcast screen|window [again] [gpu] [closed]
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <time.h>

#define BUS "org.freedesktop.impl.portal.desktop.flamewm"
#define PATH "/org/freedesktop/portal/desktop"
#define IFACE "org.freedesktop.impl.portal.ScreenCast"

static sd_bus *bus;

// restore data as it came back, to send again
struct restore {
	int has;
	uint32_t type;
	char output[128], app_id[128], title[256];
};

struct result {
	uint32_t response, node, source_type, persist;
	int32_t x, y, width, height;
	struct restore restore;
};

static int64_t now_ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int simple_call(const char *method, const char *session, uint32_t types,
		const struct restore *restore) {
	sd_bus_message *m = NULL, *reply = NULL;
	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message_new_method_call(bus, &m, BUS, PATH, IFACE, method);
	sd_bus_message_append(m, "oos", "/org/freedesktop/portal/desktop/request/1/t", session,
		"test.app");
	sd_bus_message_open_container(m, 'a', "{sv}");
	if (!strcmp(method, "SelectSources")) {
		sd_bus_message_append(m, "{sv}", "types", "u", types);
		sd_bus_message_append(m, "{sv}", "cursor_mode", "u", (uint32_t)2);
		sd_bus_message_append(m, "{sv}", "persist_mode", "u", (uint32_t)1);
		if (restore && restore->has) {
			sd_bus_message_open_container(m, 'e', "sv");
			sd_bus_message_append(m, "s", "restore_data");
			sd_bus_message_open_container(m, 'v', "(suv)");
			sd_bus_message_open_container(m, 'r', "suv");
			sd_bus_message_append(m, "su", "flamewm", (uint32_t)1);
			sd_bus_message_open_container(m, 'v', "a{sv}");
			sd_bus_message_open_container(m, 'a', "{sv}");
			sd_bus_message_append(m, "{sv}", "type", "u", restore->type);
			if (*restore->output) {
				sd_bus_message_append(m, "{sv}", "output", "s", restore->output);
			}
			if (*restore->app_id) {
				sd_bus_message_append(m, "{sv}", "app_id", "s", restore->app_id);
			}
			if (*restore->title) {
				sd_bus_message_append(m, "{sv}", "title", "s", restore->title);
			}
			for (int i = 0; i < 5; i++) {
				sd_bus_message_close_container(m);
			}
		}
	}
	sd_bus_message_close_container(m);
	int r = sd_bus_call(bus, m, 5 * 1000000, &error, &reply);
	uint32_t response = 99;
	if (r >= 0) {
		sd_bus_message_read(reply, "u", &response);
	} else {
		printf("%s failed: %s\n", method, error.message);
	}
	sd_bus_error_free(&error);
	sd_bus_message_unref(m);
	sd_bus_message_unref(reply);
	return (int)response;
}

static void read_string(sd_bus_message *m, char *out, size_t size) {
	const char *s = "";
	sd_bus_message_read(m, "v", "s", &s);
	snprintf(out, size, "%s", s);
}

static void read_restore(sd_bus_message *m, struct restore *restore) {
	const char *vendor;
	uint32_t version;
	sd_bus_message_enter_container(m, 'v', "(suv)");
	sd_bus_message_enter_container(m, 'r', "suv");
	sd_bus_message_read(m, "su", &vendor, &version);
	sd_bus_message_enter_container(m, 'v', "a{sv}");
	sd_bus_message_enter_container(m, 'a', "{sv}");
	while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
		const char *key;
		sd_bus_message_read(m, "s", &key);
		if (!strcmp(key, "type")) {
			sd_bus_message_read(m, "v", "u", &restore->type);
		} else if (!strcmp(key, "output")) {
			read_string(m, restore->output, sizeof(restore->output));
		} else if (!strcmp(key, "app_id")) {
			read_string(m, restore->app_id, sizeof(restore->app_id));
		} else if (!strcmp(key, "title")) {
			read_string(m, restore->title, sizeof(restore->title));
		} else {
			sd_bus_message_skip(m, "v");
		}
		sd_bus_message_exit_container(m);
	}
	for (int i = 0; i < 4; i++) {
		sd_bus_message_exit_container(m);
	}
	restore->has = !strcmp(vendor, "flamewm");
}

static int start(const char *session, struct result *result) {
	sd_bus_message *reply = NULL;
	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message *m = NULL;
	sd_bus_message_new_method_call(bus, &m, BUS, PATH, IFACE, "Start");
	sd_bus_message_append(m, "ooss", "/org/freedesktop/portal/desktop/request/1/s", session,
		"test.app", "");
	sd_bus_message_append(m, "a{sv}", 0);
	int r = sd_bus_call(bus, m, 30 * 1000000, &error, &reply);
	sd_bus_message_unref(m);
	if (r < 0) {
		printf("Start failed: %s\n", error.message);
		sd_bus_error_free(&error);
		return -1;
	}
	memset(result, 0, sizeof(*result));
	sd_bus_message_read(reply, "u", &result->response);
	sd_bus_message_enter_container(reply, 'a', "{sv}");
	while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
		const char *key;
		sd_bus_message_read(reply, "s", &key);
		if (!strcmp(key, "streams")) {
			sd_bus_message_enter_container(reply, 'v', "a(ua{sv})");
			sd_bus_message_enter_container(reply, 'a', "(ua{sv})");
			while (sd_bus_message_enter_container(reply, 'r', "ua{sv}") > 0) {
				sd_bus_message_read(reply, "u", &result->node);
				sd_bus_message_enter_container(reply, 'a', "{sv}");
				while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
					const char *k;
					sd_bus_message_read(reply, "s", &k);
					if (!strcmp(k, "position")) {
						sd_bus_message_read(reply, "v", "(ii)", &result->x, &result->y);
					} else if (!strcmp(k, "size")) {
						sd_bus_message_read(reply, "v", "(ii)", &result->width,
							&result->height);
					} else if (!strcmp(k, "source_type")) {
						sd_bus_message_read(reply, "v", "u", &result->source_type);
					} else {
						sd_bus_message_skip(reply, "v");
					}
					sd_bus_message_exit_container(reply);
				}
				sd_bus_message_exit_container(reply);
				sd_bus_message_exit_container(reply);
			}
			sd_bus_message_exit_container(reply);
			sd_bus_message_exit_container(reply);
		} else if (!strcmp(key, "persist_mode")) {
			sd_bus_message_read(reply, "v", "u", &result->persist);
		} else if (!strcmp(key, "restore_data")) {
			read_restore(reply, &result->restore);
		} else {
			sd_bus_message_skip(reply, "v");
		}
		sd_bus_message_exit_container(reply);
	}
	sd_bus_message_unref(reply);
	return 0;
}

static void close_session(const char *session) {
	sd_bus_call_method(bus, BUS, session, "org.freedesktop.impl.portal.Session", "Close", NULL,
		NULL, "");
}

// ---- gpu buffers, the way apps import them ----

static int gpu, closed_wait;
static EGLDisplay egl;
static uint64_t modifiers[64];
static int modifiers_len;
static PFNEGLCREATEIMAGEKHRPROC create_image;
static PFNEGLDESTROYIMAGEKHRPROC destroy_image;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_texture;

// an EGL display on the gpu, what it imports XRGB8888 as
static int gpu_init(void) {
	PFNEGLQUERYDEVICESEXTPROC query_devices =
		(void *)eglGetProcAddress("eglQueryDevicesEXT");
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
		(void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
	PFNEGLQUERYDMABUFMODIFIERSEXTPROC query_modifiers =
		(void *)eglGetProcAddress("eglQueryDmaBufModifiersEXT");
	create_image = (void *)eglGetProcAddress("eglCreateImageKHR");
	destroy_image = (void *)eglGetProcAddress("eglDestroyImageKHR");
	image_texture = (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");
	EGLDeviceEXT devices[8];
	EGLint n = 0;
	if (!query_devices || !get_display || !query_modifiers || !create_image || !image_texture
			|| !query_devices(8, devices, &n) || n < 1) {
		return -1;
	}
	egl = get_display(EGL_PLATFORM_DEVICE_EXT, devices[0], NULL);
	if (!eglInitialize(egl, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API)) {
		return -1;
	}
	EGLint attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
	EGLContext context = eglCreateContext(egl, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attribs);
	if (context == EGL_NO_CONTEXT
			|| !eglMakeCurrent(egl, EGL_NO_SURFACE, EGL_NO_SURFACE, context)) {
		return -1;
	}
	EGLBoolean external[64];
	EGLint len = 0;
	query_modifiers(egl, DRM_FORMAT_XRGB8888, 64, (EGLuint64KHR *)modifiers, external, &len);
	// only what it can draw from as a plain texture, to read it back simply
	for (EGLint i = 0; i < len; i++) {
		if (!external[i]) {
			modifiers[modifiers_len++] = modifiers[i];
		}
	}
	return modifiers_len ? 0 : -1;
}

// the buffer's middle pixel, read on the gpu: imported, drawn into nothing,
// its texture read through a framebuffer
static int gpu_pixel(const struct spa_buffer *buffer, int width, int height, uint64_t modifier,
		uint32_t *pixel) {
	const struct spa_data *d = &buffer->datas[0];
	EGLAttrib attribs[] = {
		EGL_WIDTH, width, EGL_HEIGHT, height,
		EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_XRGB8888,
		EGL_DMA_BUF_PLANE0_FD_EXT, (EGLAttrib)d->fd,
		EGL_DMA_BUF_PLANE0_OFFSET_EXT, d->chunk->offset,
		EGL_DMA_BUF_PLANE0_PITCH_EXT, d->chunk->stride,
		EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLAttrib)(modifier & 0xffffffff),
		EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLAttrib)(modifier >> 32),
		EGL_NONE,
	};
	EGLint int_attribs[sizeof(attribs) / sizeof(attribs[0])];
	for (size_t i = 0; i < sizeof(attribs) / sizeof(attribs[0]); i++) {
		int_attribs[i] = (EGLint)attribs[i];
	}
	EGLImageKHR image = create_image(egl, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL,
		int_attribs);
	if (image == EGL_NO_IMAGE_KHR) {
		return -1;
	}
	GLuint texture, fbo;
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	image_texture(GL_TEXTURE_2D, image);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
	int ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
	uint8_t rgba[4] = {0};
	if (ok) {
		glReadPixels(width / 2, height / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
		*pixel = (uint32_t)rgba[0] << 16 | (uint32_t)rgba[1] << 8 | rgba[2];
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glDeleteFramebuffers(1, &fbo);
	glDeleteTextures(1, &texture);
	destroy_image(egl, image);
	return ok ? 0 : -1;
}

// ---- taking the stream like an app ----

struct watch {
	struct pw_main_loop *loop;
	struct pw_stream *stream;
	struct spa_hook listener;
	struct spa_video_info_raw format;
	int dmabuf;
	int frames;
	uint32_t pixel;
	int has_pixel;
};

static void on_param_changed(void *data, uint32_t id, const struct spa_pod *param) {
	struct watch *watch = data;
	if (id != SPA_PARAM_Format || !param) {
		return;
	}
	spa_format_video_raw_parse(param, &watch->format);
	const struct spa_pod_prop *modifier = spa_pod_find_prop(param, NULL,
		SPA_FORMAT_VIDEO_modifier);
	watch->dmabuf = modifier && !(modifier->flags & SPA_POD_PROP_FLAG_DONT_FIXATE);
	if (modifier && !watch->dmabuf) {
		return; // the producer fixes it, the format comes again
	}
	uint8_t buf[256];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	const struct spa_pod *params[1];
	params[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(watch->dmabuf
			? 1 << SPA_DATA_DmaBuf : 1 << SPA_DATA_MemFd));
	pw_stream_update_params(watch->stream, params, 1);
}

static void on_process(void *data) {
	struct watch *watch = data;
	struct pw_buffer *b = pw_stream_dequeue_buffer(watch->stream);
	if (!b) {
		return;
	}
	struct spa_data *d = &b->buffer->datas[0];
	if (d->type == SPA_DATA_DmaBuf) {
		uint32_t pixel;
		if (gpu_pixel(b->buffer, (int)watch->format.size.width, (int)watch->format.size.height,
				watch->format.modifier, &pixel) == 0) {
			watch->frames++;
			watch->pixel = pixel;
			watch->has_pixel = 1;
		}
	} else if (d->data && d->chunk->size > 0) {
		watch->frames++;
		int w = (int)watch->format.size.width, h = (int)watch->format.size.height;
		const uint8_t *row = (const uint8_t *)d->data + d->chunk->offset
			+ (size_t)(h / 2) * (size_t)d->chunk->stride;
		const uint8_t *px = row + (size_t)(w / 2) * 4;
		// BGRx in memory
		watch->pixel = (uint32_t)px[2] << 16 | (uint32_t)px[1] << 8 | px[0];
		watch->has_pixel = 1;
	}
	pw_stream_queue_buffer(watch->stream, b);
}

static const struct pw_stream_events watch_events = {
	PW_VERSION_STREAM_EVENTS,
	.param_changed = on_param_changed,
	.process = on_process,
};

static void on_timeout(void *data, uint64_t expirations) {
	pw_main_loop_quit(data);
}

static void watch_stream(uint32_t node, const char *label) {
	struct watch watch = {0};
	watch.loop = pw_main_loop_new(NULL);
	struct pw_context *context = pw_context_new(pw_main_loop_get_loop(watch.loop), NULL, 0);
	struct pw_core *core = pw_context_connect(context, NULL, 0);
	if (!core) {
		printf("%s: no pipewire\n", label);
		return;
	}
	watch.stream = pw_stream_new(core, "portalcast",
		pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
			PW_KEY_MEDIA_ROLE, "Screen", NULL));
	pw_stream_add_listener(watch.stream, &watch.listener, &watch_events, &watch);
	uint8_t buf[4096];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	const struct spa_pod *params[2];
	uint32_t n = 0;
	if (gpu) {
		// the modifiers this gpu imports, the producer picks one
		struct spa_pod_frame f[2];
		spa_pod_builder_push_object(&b, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
		spa_pod_builder_add(&b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
		spa_pod_builder_add(&b, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
		spa_pod_builder_add(&b, SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_BGRx), 0);
		spa_pod_builder_prop(&b, SPA_FORMAT_VIDEO_modifier,
			SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
		spa_pod_builder_push_choice(&b, &f[1], SPA_CHOICE_Enum, 0);
		spa_pod_builder_long(&b, (int64_t)modifiers[0]);
		for (int i = 0; i < modifiers_len; i++) {
			spa_pod_builder_long(&b, (int64_t)modifiers[i]);
		}
		spa_pod_builder_pop(&b, &f[1]);
		spa_pod_builder_add(&b, SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(
			&SPA_RECTANGLE(320, 240), &SPA_RECTANGLE(1, 1), &SPA_RECTANGLE(8192, 8192)), 0);
		spa_pod_builder_add(&b, SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(
			&SPA_FRACTION(0, 1), &SPA_FRACTION(0, 1), &SPA_FRACTION(1000, 1)), 0);
		params[n++] = spa_pod_builder_pop(&b, &f[0]);
	}
	params[n++] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
		SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
		SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(3, SPA_VIDEO_FORMAT_BGRx,
			SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA),
		SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&SPA_RECTANGLE(320, 240),
			&SPA_RECTANGLE(1, 1), &SPA_RECTANGLE(8192, 8192)),
		SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&SPA_FRACTION(0, 1),
			&SPA_FRACTION(0, 1), &SPA_FRACTION(1000, 1)));
	pw_stream_connect(watch.stream, PW_DIRECTION_INPUT, node,
		PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, params, n);
	struct spa_source *timer = pw_loop_add_timer(pw_main_loop_get_loop(watch.loop), on_timeout,
		watch.loop);
	struct timespec two = {.tv_sec = 2};
	pw_loop_update_timer(pw_main_loop_get_loop(watch.loop), timer, &two, NULL, false);
	pw_main_loop_run(watch.loop);
	printf("%s: frames %s, %ux%u%s\n", label, watch.frames > 0 ? "came" : "none",
		watch.format.size.width, watch.format.size.height,
		watch.dmabuf ? " in gpu buffers" : " in shared memory");
	if (watch.has_pixel) {
		printf("%s: middle pixel %06x\n", label, watch.pixel);
	}
	pw_stream_destroy(watch.stream);
	pw_core_disconnect(core);
	pw_context_destroy(context);
	pw_main_loop_destroy(watch.loop);
}

static int on_closed(sd_bus_message *m, void *data, sd_bus_error *error) {
	*(int *)data = 1;
	return 0;
}

// flame says the share's over, within 5s
static void wait_closed(const char *session, const char *label) {
	int closed = 0;
	sd_bus_slot *slot = NULL;
	sd_bus_match_signal(bus, &slot, BUS, session, "org.freedesktop.impl.portal.Session",
		"Closed", on_closed, &closed);
	int64_t until = now_ms() + 5000;
	while (!closed && now_ms() < until) {
		if (sd_bus_process(bus, NULL) == 0) {
			sd_bus_wait(bus, 100000);
		}
	}
	printf("%s: %s\n", label, closed ? "closed by flame" : "never closed");
	sd_bus_slot_unref(slot);
}

static int share(const char *session, uint32_t types, const struct restore *restore,
		struct result *result, const char *label) {
	if (simple_call("CreateSession", session, 0, NULL) != 0
			|| simple_call("SelectSources", session, types, restore) != 0) {
		printf("%s: session refused\n", label);
		return -1;
	}
	int64_t asked = now_ms();
	if (start(session, result) < 0) {
		return -1;
	}
	printf("%s: start %u%s\n", label, result->response,
		now_ms() - asked < 300 ? ", right away" : "");
	if (result->response != 0) {
		return -1;
	}
	printf("%s: source %s, %dx%d at %d,%d, kept %s\n", label,
		result->source_type == 1 ? "screen" : result->source_type == 2 ? "window" : "?",
		result->width, result->height, result->x, result->y,
		result->persist && result->restore.has ? "yes" : "no");
	if (closed_wait) {
		wait_closed(session, label);
	} else {
		watch_stream(result->node, label);
	}
	return 0;
}

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: portalcast screen|window [again]\n");
		return 1;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	pw_init(&argc, &argv);
	if (sd_bus_open_user(&bus) < 0) {
		printf("no session bus\n");
		return 1;
	}
	uint32_t types = !strcmp(argv[1], "window") ? 2 : 1;
	int again = 0;
	for (int i = 2; i < argc; i++) {
		again |= !strcmp(argv[i], "again");
		gpu |= !strcmp(argv[i], "gpu");
		closed_wait |= !strcmp(argv[i], "closed");
	}
	if (gpu && gpu_init() < 0) {
		printf("no gpu to import with\n");
		return 1;
	}
	struct result first;
	if (share("/org/freedesktop/portal/desktop/session/1/one", types, NULL, &first,
			"first") < 0) {
		return 1;
	}
	if (again) {
		struct result second;
		share("/org/freedesktop/portal/desktop/session/1/two", types, &first.restore, &second,
			"again");
		close_session("/org/freedesktop/portal/desktop/session/1/two");
	}
	close_session("/org/freedesktop/portal/desktop/session/1/one");
	printf("done\n");
	return 0;
}
