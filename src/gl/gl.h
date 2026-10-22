#ifndef FLAME_GL_GL_H
#define FLAME_GL_GL_H

#include <pixman.h>
#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-protocol.h>
#include <wlr/util/box.h>

struct wlr_buffer;
struct wlr_drm_syncobj_timeline;
struct wlr_renderer;
struct wlr_texture;

// flame's own renderer: every frame flame composes is drawn by flame's
// shaders, in the GL context of wlroots' renderer. wlroots still turns
// client buffers into textures and output buffers into framebuffers, flame
// does the drawing. all coords are buffer pixels, the top row is y 0

struct gl_program {
	unsigned program;
	int proj, tex_proj, tex, alpha, color; // uniforms, -1 for none
	int pos; // the vertex attribute
};

struct flame_gl {
	bool usable;
	struct wlr_renderer *renderer;
	void *display, *context; // EGLDisplay, EGLContext
	bool external; // GL_OES_EGL_image_external: dmabufs as external textures
	bool fences; // EGL_ANDROID_native_fence_sync + EGL_KHR_wait_sync
	struct gl_program quad, tex_rgba, tex_rgbx, tex_ext;
};

// false (and unusable) when renderer isn't GLES2 or its GL lacks what
// flame needs, the caller draws some other way then
bool gl_init(struct flame_gl *gl, struct wlr_renderer *renderer);
void gl_finish(struct flame_gl *gl);

// one buffer being drawn, on the caller's stack
struct gl_pass {
	struct flame_gl *gl;
	unsigned fbo;
	int width, height;
	float proj[9];
	struct wlr_drm_syncobj_timeline *signal;
	uint64_t signal_point;
};

// signal: reaches point once the gpu is done with everything drawn, NULL
// for implicit sync
bool gl_pass_begin(struct gl_pass *pass, struct flame_gl *gl, struct wlr_buffer *buffer,
	struct wlr_drm_syncobj_timeline *signal, uint64_t point);
// color premultiplied. blend off: it replaces what's there
void gl_pass_rect(struct gl_pass *pass, struct wlr_box box, const float color[4],
	const pixman_region32_t *clip, bool blend);

struct gl_texture_options {
	struct wlr_texture *texture;
	struct wlr_fbox src; // texture pixels, empty for all of it
	struct wlr_box dst;
	// how the texture lies in dst, as wlroots' render pass takes it
	enum wl_output_transform transform;
	const pixman_region32_t *clip; // NULL for all of dst
	float alpha; // 1 for opaque
	bool blend; // premultiplied over what's there, off replaces it
	bool nearest; // pixel exact scaling, bilinear otherwise
	// the gpu work that made it, waited on before it's read. NULL: none
	struct wlr_drm_syncobj_timeline *wait_timeline;
	uint64_t wait_point;
};

void gl_pass_texture(struct gl_pass *pass, const struct gl_texture_options *options);
// false: the gpu can't be trusted with what it drew
bool gl_pass_submit(struct gl_pass *pass);

#endif
