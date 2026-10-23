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
	int dst, shape, shape_radius; // textures cut to a rounded window
	int pos; // the vertex attribute
	// glass and blur
	int texel, offset, bounds, box, reach, radius, tint, saturation, edge, refraction,
		rim;
};

// what glass reads from: what's behind it, copied out, then shrunk and
// grown back level by level (dual kawase), the blur ends up in level 0.
// kept between frames, grown when a bigger glass needs more
#define GL_BLUR_LEVELS 7
struct gl_blur {
	unsigned texture[GL_BLUR_LEVELS], fbo[GL_BLUR_LEVELS];
	int width[GL_BLUR_LEVELS], height[GL_BLUR_LEVELS]; // allocated
	// the copy itself: rgb, an output's buffer often has no alpha to copy
	unsigned copy;
	int copy_width, copy_height;
};

struct flame_gl {
	bool usable;
	struct wlr_renderer *renderer;
	void *display, *context; // EGLDisplay, EGLContext
	bool external; // GL_OES_EGL_image_external: dmabufs as external textures
	bool fences; // EGL_ANDROID_native_fence_sync + EGL_KHR_wait_sync
	struct gl_program quad, tex_rgba, tex_rgbx, tex_ext;
	struct gl_program blur_down, blur_up, glass;
	struct gl_blur blur;
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
	// glass reads this instead of what's drawn now, see gl_pass_backdrop.
	// empty for none
	struct wlr_box backdrop;
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
	// the rounded window it's part of (buffer pixels): outside it, nothing
	// shows. a radius of 0 for none
	struct wlr_box shape;
	float shape_radius;
	// the gpu work that made it, waited on before it's read. NULL: none
	struct wlr_drm_syncobj_timeline *wait_timeline;
	uint64_t wait_point;
};

void gl_pass_texture(struct gl_pass *pass, const struct gl_texture_options *options);

// buffer pixels, like everything else in a pass
struct gl_glass_options {
	struct wlr_box box;
	const pixman_region32_t *clip; // NULL for all of box
	float radius, blur, refraction;
	float tint[4]; // premultiplied
	float saturation, edge;
	float rim; // how wide the lit rim is
	float alpha; // the whole pane faded, 1 for not at all
};

// glass over what this pass drew so far. it reads box grown by
// gl_glass_reach: all of that has to be drawn already, this frame
void gl_pass_glass(struct gl_pass *pass, const struct gl_glass_options *options);
// how far around its box glass reads, buffer pixels
int gl_glass_reach(float blur, float refraction);
// what's in area now is kept, and glass reads that from here on instead of
// what's drawn over it since: a window's panes all frost what's behind the
// window, not each other. area: every pane's box and reach. an empty area
// (gl_pass_backdrop_end) goes back to reading what's drawn
void gl_pass_backdrop(struct gl_pass *pass, struct wlr_box area);
void gl_pass_backdrop_end(struct gl_pass *pass);
// false: the gpu can't be trusted with what it drew
bool gl_pass_submit(struct gl_pass *pass);

#endif
