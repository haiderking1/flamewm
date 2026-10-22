// flame's renderer (gl/) against wlroots' render pass: the same calls into
// two buffers, every pixel compared. rotations and flips, cropped sources,
// clips, alpha, blending, textures with and without alpha. needs a gpu with
// GLES2, skipped without one
#include <drm_fourcc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/gles2.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/render/swapchain.h>
#include <wlr/util/log.h>

#include "gl/gl.h"

#define W 96
#define H 64

static struct wlr_renderer *renderer;
static struct wlr_allocator *allocator;
static struct flame_gl gl;
static int failed;

// what a headless output renders to: a format and modifiers the gpu draws
// into (nvidia can't draw to linear)
static struct wlr_drm_format *format;

static struct wlr_buffer *new_buffer(void) {
	return wlr_allocator_create_buffer(allocator, W, H, format);
}

static bool read_back(struct wlr_buffer *buffer, uint32_t *out) {
	struct wlr_texture *texture = wlr_texture_from_buffer(renderer, buffer);
	bool ok = texture && wlr_texture_read_pixels(texture, &(struct wlr_texture_read_pixels_options){
		.data = out,
		.format = DRM_FORMAT_ARGB8888,
		.stride = W * 4,
	});
	if (texture) {
		wlr_texture_destroy(texture);
	}
	return ok;
}

// a texture nothing about is symmetric: every pixel its own color
static struct wlr_texture *pattern(int width, int height, bool alpha) {
	uint32_t *pixels = malloc((size_t)width * height * 4);
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			uint32_t a = alpha ? (uint32_t)(64 + (x * 191) / width) : 0xff;
			uint32_t r = (uint32_t)(x * 255 / width) * a / 255;
			uint32_t g = (uint32_t)(y * 255 / height) * a / 255;
			uint32_t b = (uint32_t)((x + y) % 7 * 36) * a / 255;
			pixels[y * width + x] = a << 24 | r << 16 | g << 8 | b;
		}
	}
	struct wlr_texture *texture = wlr_texture_from_pixels(renderer,
		alpha ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888, (uint32_t)width * 4,
		(uint32_t)width, (uint32_t)height, pixels);
	free(pixels);
	return texture;
}

struct step {
	bool is_rect;
	struct wlr_box box;
	float color[4];
	struct gl_texture_options texture;
	bool blend;
	pixman_region32_t *clip;
};

static void draw_ours(struct wlr_buffer *buffer, const struct step *steps, int len) {
	struct gl_pass pass;
	if (!gl_pass_begin(&pass, &gl, buffer, NULL, 0)) {
		fprintf(stderr, "gl: no pass\n");
		failed++;
		return;
	}
	for (int i = 0; i < len; i++) {
		if (steps[i].is_rect) {
			gl_pass_rect(&pass, steps[i].box, steps[i].color, steps[i].clip, steps[i].blend);
		} else {
			struct gl_texture_options options = steps[i].texture;
			options.clip = steps[i].clip;
			gl_pass_texture(&pass, &options);
		}
	}
	gl_pass_submit(&pass);
}

static void draw_wlroots(struct wlr_buffer *buffer, const struct step *steps, int len) {
	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(renderer, buffer, NULL);
	if (!pass) {
		fprintf(stderr, "wlroots: no pass\n");
		failed++;
		return;
	}
	for (int i = 0; i < len; i++) {
		if (steps[i].is_rect) {
			const float *c = steps[i].color;
			wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
				.box = steps[i].box,
				.color = {c[0], c[1], c[2], c[3]},
				.clip = steps[i].clip,
				.blend_mode = steps[i].blend ? WLR_RENDER_BLEND_MODE_PREMULTIPLIED
					: WLR_RENDER_BLEND_MODE_NONE,
			});
		} else {
			const struct gl_texture_options *o = &steps[i].texture;
			wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
				.texture = o->texture,
				.src_box = o->src,
				.dst_box = o->dst,
				.transform = o->transform,
				.clip = steps[i].clip,
				.alpha = o->alpha < 1.0f ? &o->alpha : NULL,
				.blend_mode = o->blend ? WLR_RENDER_BLEND_MODE_PREMULTIPLIED
					: WLR_RENDER_BLEND_MODE_NONE,
				.filter_mode = o->nearest ? WLR_SCALE_FILTER_NEAREST
					: WLR_SCALE_FILTER_BILINEAR,
			});
		}
	}
	wlr_render_pass_submit(pass);
}

// a grey ground first, so blending shows
static void compare(const char *name, const struct step *steps, int len) {
	struct step all[16];
	all[0] = (struct step){.is_rect = true, .box = {0, 0, W, H},
		.color = {0.25f, 0.5f, 0.75f, 1}};
	memcpy(&all[1], steps, sizeof(*steps) * (size_t)len);
	struct wlr_buffer *a = new_buffer(), *b = new_buffer();
	draw_ours(a, all, len + 1);
	draw_wlroots(b, all, len + 1);
	static uint32_t pa[W * H], pb[W * H];
	if (!read_back(a, pa) || !read_back(b, pb)) {
		fprintf(stderr, "%s: can't read back\n", name);
		failed++;
	} else {
		int differ = 0, worst = 0;
		for (int i = 0; i < W * H; i++) {
			for (int s = 0; s < 32; s += 8) {
				int d = abs((int)(pa[i] >> s & 0xff) - (int)(pb[i] >> s & 0xff));
				worst = d > worst ? d : worst;
				differ += d > 0;
			}
		}
		// bilinear sampling may round a channel differently, never more
		if (worst > 1) {
			fprintf(stderr, "%s: %d channels differ, by up to %d\n", name, differ, worst);
			failed++;
		}
	}
	wlr_buffer_drop(a);
	wlr_buffer_drop(b);
}

static void test_rects(void) {
	pixman_region32_t clip;
	pixman_region32_init_rect(&clip, 10, 5, 30, 20);
	pixman_region32_union_rect(&clip, &clip, 50, 30, 20, 25);
	struct step steps[] = {
		{.is_rect = true, .box = {5, 5, 40, 30}, .color = {1, 0, 0, 1}, .blend = true},
		{.is_rect = true, .box = {20, 10, 50, 40}, .color = {0, 0.4f, 0, 0.4f}, .blend = true},
		{.is_rect = true, .box = {0, 0, 80, 60}, .color = {0, 0, 0.5f, 0.5f}, .blend = false,
			.clip = &clip},
		{.is_rect = true, .box = {60, 2, 30, 30}, .color = {0, 0, 0, 0}, .blend = false},
	};
	compare("rects", steps, 4);
	pixman_region32_fini(&clip);
}

static void test_textures(void) {
	const char *names[8] = {"normal", "90", "180", "270", "flipped", "flipped-90",
		"flipped-180", "flipped-270"};
	struct wlr_texture *rgba = pattern(40, 24, true), *rgbx = pattern(40, 24, false);
	pixman_region32_t clip;
	pixman_region32_init_rect(&clip, 8, 8, 30, 12);
	pixman_region32_union_rect(&clip, &clip, 40, 20, 40, 40);
	for (int t = 0; t < 8; t++) {
		bool quarter = t & 1;
		struct wlr_box dst = {6, 4, quarter ? 24 : 40, quarter ? 40 : 24};
		struct wlr_box big = {30, 10, quarter ? 36 : 60, quarter ? 60 : 36}; // scaled up
		char name[64];
		struct step steps[] = {
			{.texture = {.texture = rgba, .dst = dst, .transform = t, .alpha = 1,
				.blend = true, .nearest = true}},
			{.texture = {.texture = rgbx, .dst = big, .transform = t, .alpha = 1,
				.blend = true, .nearest = true,
				.src = {4, 2, 30, 18}}},
		};
		snprintf(name, sizeof(name), "textures %s", names[t]);
		compare(name, steps, 2);
		struct step faded[] = {
			{.texture = {.texture = rgba, .dst = big, .transform = t, .alpha = 0.6f,
				.blend = true, .nearest = true}},
			{.texture = {.texture = rgbx, .dst = dst, .transform = t, .alpha = 0.5f,
				.blend = true, .nearest = true}},
		};
		faded[0].clip = &clip;
		snprintf(name, sizeof(name), "faded and clipped %s", names[t]);
		compare(name, faded, 2);
		struct step smooth[] = {
			{.texture = {.texture = rgba, .dst = big, .transform = t, .alpha = 1,
				.blend = true, .src = {3.5, 1.25, 20, 13}}},
		};
		snprintf(name, sizeof(name), "bilinear %s", names[t]);
		compare(name, smooth, 1);
	}
	struct step replace[] = {
		{.texture = {.texture = rgba, .dst = {10, 10, 40, 24}, .alpha = 1, .blend = false,
			.nearest = true}},
	};
	compare("no blending", replace, 1);
	pixman_region32_fini(&clip);
	wlr_texture_destroy(rgba);
	wlr_texture_destroy(rgbx);
}

int main(void) {
	wlr_log_init(WLR_ERROR, NULL);
	struct wl_display *display = wl_display_create();
	struct wlr_backend *backend = wlr_headless_backend_create(wl_display_get_event_loop(display));
	renderer = backend ? wlr_renderer_autocreate(backend) : NULL;
	if (!renderer || !wlr_renderer_is_gles2(renderer)) {
		printf("gl: skipped, no GLES2 renderer here\n");
		return 0;
	}
	allocator = wlr_allocator_autocreate(backend, renderer);
	struct wlr_output *output = allocator ? wlr_headless_add_output(backend, W, H) : NULL;
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	wlr_output_state_set_custom_mode(&state, W, H, 0);
	if (!output || !wlr_output_init_render(output, allocator, renderer)
			|| !wlr_output_configure_primary_swapchain(output, &state, &output->swapchain)
			|| !gl_init(&gl, renderer)) {
		fprintf(stderr, "gl: can't set up\n");
		return 1;
	}
	wlr_output_state_finish(&state);
	format = &output->swapchain->format;
	test_rects();
	test_textures();
	gl_finish(&gl);
	wlr_allocator_destroy(allocator);
	wlr_renderer_destroy(renderer);
	wlr_backend_destroy(backend);
	wl_display_destroy(display);
	if (failed) {
		fprintf(stderr, "gl: %d failed\n", failed);
		return 1;
	}
	printf("gl: all good, flame draws what wlroots draws\n");
	return 0;
}
