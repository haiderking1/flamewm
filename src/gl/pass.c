#include "gl/gl.h"

#include <string.h>
#include <unistd.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/render/gles2.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/log.h>
#include <wlr/util/transform.h>

#include "gl/internal.h"

// 3x3 matrices, row major, as the shaders take them (pos3 * m)

static void mat_identity(float m[9]) {
	static const float id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
	memcpy(m, id, sizeof(id));
}

static void mat_multiply(float out[9], const float a[9], const float b[9]) {
	float r[9];
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			r[i * 3 + j] = a[i * 3 + 0] * b[0 * 3 + j] + a[i * 3 + 1] * b[1 * 3 + j]
				+ a[i * 3 + 2] * b[2 * 3 + j];
		}
	}
	memcpy(out, r, sizeof(r));
}

static void mat_translate(float m[9], float x, float y) {
	float t[9] = {1, 0, x, 0, 1, y, 0, 0, 1};
	mat_multiply(m, m, t);
}

static void mat_scale(float m[9], float x, float y) {
	float s[9] = {x, 0, 0, 0, y, 0, 0, 0, 1};
	mat_multiply(m, m, s);
}

// the unit square turned the way a wl_output_transform turns it
static void mat_transform(float m[9], enum wl_output_transform transform) {
	static const float transforms[8][9] = {
		[WL_OUTPUT_TRANSFORM_NORMAL] = {1, 0, 0, 0, 1, 0, 0, 0, 1},
		[WL_OUTPUT_TRANSFORM_90] = {0, 1, 0, -1, 0, 0, 0, 0, 1},
		[WL_OUTPUT_TRANSFORM_180] = {-1, 0, 0, 0, -1, 0, 0, 0, 1},
		[WL_OUTPUT_TRANSFORM_270] = {0, -1, 0, 1, 0, 0, 0, 0, 1},
		[WL_OUTPUT_TRANSFORM_FLIPPED] = {-1, 0, 0, 0, 1, 0, 0, 0, 1},
		[WL_OUTPUT_TRANSFORM_FLIPPED_90] = {0, 1, 0, 1, 0, 0, 0, 0, 1},
		[WL_OUTPUT_TRANSFORM_FLIPPED_180] = {1, 0, 0, 0, -1, 0, 0, 0, 1},
		[WL_OUTPUT_TRANSFORM_FLIPPED_270] = {0, -1, 0, -1, 0, 0, 0, 0, 1},
	};
	mat_multiply(m, m, transforms[transform & 7]);
}

// the unit square onto box, in the buffer
static void set_proj(const struct gl_pass *pass, int location, struct wlr_box box) {
	float m[9];
	mat_identity(m);
	mat_translate(m, box.x, box.y);
	mat_scale(m, box.width, box.height);
	mat_multiply(m, pass->proj, m);
	fglUniformMatrix3fv(location, 1, GL_FALSE, m);
}

// the unit square of dst onto the texture's src, normalized, turned. a
// quarter turn goes the other way on textures, their origin differs
static void set_tex_proj(int location, enum wl_output_transform transform,
		struct wlr_fbox src) {
	float m[9];
	mat_identity(m);
	mat_translate(m, src.x, src.y);
	mat_scale(m, src.width, src.height);
	mat_translate(m, 0.5f, 0.5f);
	mat_transform(m, transform & WL_OUTPUT_TRANSFORM_90
		? wlr_output_transform_invert(transform) : transform);
	mat_translate(m, -0.5f, -0.5f);
	fglUniformMatrix3fv(location, 1, GL_FALSE, m);
}

#define MAX_QUADS 86 // a batch of quads, 6 vertices of 2 floats each

// the parts of box inside clip, as quads in box's unit square
static void draw_box(struct wlr_box box, const pixman_region32_t *clip, int pos) {
	pixman_region32_t region;
	pixman_region32_init_rect(&region, box.x, box.y, box.width, box.height);
	if (clip) {
		pixman_region32_intersect(&region, &region, clip);
	}
	int len;
	const pixman_box32_t *rects = pixman_region32_rectangles(&region, &len);
	if (len == 0) {
		pixman_region32_fini(&region);
		return;
	}
	fglEnableVertexAttribArray(pos);
	for (int i = 0; i < len;) {
		int batch = len - i < MAX_QUADS ? len - i : MAX_QUADS;
		GLfloat verts[MAX_QUADS * 12];
		size_t n = 0;
		for (int end = i + batch; i < end; i++) {
			GLfloat x1 = (GLfloat)(rects[i].x1 - box.x) / box.width;
			GLfloat y1 = (GLfloat)(rects[i].y1 - box.y) / box.height;
			GLfloat x2 = (GLfloat)(rects[i].x2 - box.x) / box.width;
			GLfloat y2 = (GLfloat)(rects[i].y2 - box.y) / box.height;
			GLfloat quad[12] = {x1, y1, x2, y1, x1, y2, x2, y1, x2, y2, x1, y2};
			memcpy(&verts[n], quad, sizeof(quad));
			n += 12;
		}
		fglVertexAttribPointer(pos, 2, GL_FLOAT, GL_FALSE, 0, verts);
		fglDrawArrays(GL_TRIANGLES, 0, batch * 6);
	}
	fglDisableVertexAttribArray(pos);
	pixman_region32_fini(&region);
}

static void set_blend(bool blend) {
	if (blend) {
		fglEnable(GL_BLEND);
	} else {
		fglDisable(GL_BLEND);
	}
}

bool gl_pass_begin(struct gl_pass *pass, struct flame_gl *gl, struct wlr_buffer *buffer,
		struct wlr_drm_syncobj_timeline *signal, uint64_t point) {
	memset(pass, 0, sizeof(*pass));
	if (!gl->usable || (signal && !gl->fences)) {
		return false;
	}
	gl_make_current(gl);
	GLuint fbo = wlr_gles2_renderer_get_buffer_fbo(gl->renderer, buffer);
	if (!fbo) {
		return false;
	}
	pass->gl = gl;
	pass->fbo = fbo;
	pass->width = buffer->width;
	pass->height = buffer->height;
	pass->signal = signal;
	pass->signal_point = point;
	// buffer pixels to clip space, the top row first in memory: y down
	// maps straight onto GL's y up, no flip
	mat_identity(pass->proj);
	pass->proj[0] = 2.0f / buffer->width;
	pass->proj[2] = -1;
	pass->proj[4] = 2.0f / buffer->height;
	pass->proj[5] = -1;
	fglBindFramebuffer(GL_FRAMEBUFFER, fbo);
	fglViewport(0, 0, buffer->width, buffer->height);
	fglBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	fglDisable(GL_SCISSOR_TEST);
	return true;
}

void gl_pass_rect(struct gl_pass *pass, struct wlr_box box, const float color[4],
		const pixman_region32_t *clip, bool blend) {
	if (wlr_box_empty(&box)) {
		return;
	}
	blend = blend && color[3] < 1.0f;
	if (!blend && !clip && box.x == 0 && box.y == 0 && box.width == pass->width
			&& box.height == pass->height) {
		fglClearColor(color[0], color[1], color[2], color[3]);
		fglClear(GL_COLOR_BUFFER_BIT);
		return;
	}
	const struct gl_program *program = &pass->gl->quad;
	set_blend(blend);
	fglUseProgram(program->program);
	set_proj(pass, program->proj, box);
	fglUniform4f(program->color, color[0], color[1], color[2], color[3]);
	draw_box(box, clip, program->pos);
}

// the gpu waits (not us) for point on timeline before what comes after
static bool gpu_wait(struct flame_gl *gl, struct wlr_drm_syncobj_timeline *timeline,
		uint64_t point) {
	int fd = wlr_drm_syncobj_timeline_export_sync_file(timeline, point);
	if (fd < 0) {
		return false;
	}
	EGLint attribs[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fd, EGL_NONE};
	EGLSyncKHR sync = feglCreateSyncKHR(gl->display, EGL_SYNC_NATIVE_FENCE_ANDROID, attribs);
	if (sync == EGL_NO_SYNC_KHR) {
		close(fd); // EGL only owns it once the sync exists
		return false;
	}
	bool ok = feglWaitSyncKHR(gl->display, sync, 0) == EGL_TRUE;
	feglDestroySyncKHR(gl->display, sync);
	return ok;
}

void gl_pass_texture(struct gl_pass *pass, const struct gl_texture_options *options) {
	struct wlr_texture *texture = options->texture;
	if (!texture || wlr_box_empty(&options->dst) || !wlr_texture_is_gles2(texture)) {
		return;
	}
	struct wlr_gles2_texture_attribs attribs;
	wlr_gles2_texture_get_attribs(texture, &attribs);
	const struct gl_program *program;
	if (attribs.target == GL_TEXTURE_EXTERNAL_OES) {
		if (!pass->gl->external) {
			return;
		}
		program = &pass->gl->tex_ext;
	} else {
		program = attribs.has_alpha ? &pass->gl->tex_rgba : &pass->gl->tex_rgbx;
	}
	if (options->wait_timeline && (!pass->gl->fences
			|| !gpu_wait(pass->gl, options->wait_timeline, options->wait_point))) {
		return; // never read it half drawn
	}

	struct wlr_fbox src = options->src;
	if (wlr_fbox_empty(&src)) {
		src = (struct wlr_fbox){0, 0, texture->width, texture->height};
	}
	src.x /= texture->width;
	src.y /= texture->height;
	src.width /= texture->width;
	src.height /= texture->height;

	set_blend((attribs.has_alpha || options->alpha < 1.0f) && options->blend);
	fglUseProgram(program->program);
	fglActiveTexture(GL_TEXTURE0);
	fglBindTexture(attribs.target, attribs.tex);
	GLint filter = options->nearest ? GL_NEAREST : GL_LINEAR;
	fglTexParameteri(attribs.target, GL_TEXTURE_MIN_FILTER, filter);
	fglTexParameteri(attribs.target, GL_TEXTURE_MAG_FILTER, filter);
	fglUniform1i(program->tex, 0);
	fglUniform1f(program->alpha, options->alpha);
	set_proj(pass, program->proj, options->dst);
	set_tex_proj(program->tex_proj, options->transform, src);
	draw_box(options->dst, options->clip, program->pos);
	fglBindTexture(attribs.target, 0);
}

bool gl_pass_submit(struct gl_pass *pass) {
	struct flame_gl *gl = pass->gl;
	bool ok = true;
	if (pass->signal) {
		// a fence after everything drawn, its fd becomes the timeline point
		EGLint attribs[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, EGL_NO_NATIVE_FENCE_FD_ANDROID,
			EGL_NONE};
		EGLSyncKHR sync = feglCreateSyncKHR(gl->display, EGL_SYNC_NATIVE_FENCE_ANDROID,
			attribs);
		fglFlush();
		int fd = sync != EGL_NO_SYNC_KHR
			? feglDupNativeFenceFDANDROID(gl->display, sync) : EGL_NO_NATIVE_FENCE_FD_ANDROID;
		if (sync != EGL_NO_SYNC_KHR) {
			feglDestroySyncKHR(gl->display, sync);
		}
		ok = fd >= 0 && wlr_drm_syncobj_timeline_import_sync_file(pass->signal,
			pass->signal_point, fd);
		if (fd >= 0) {
			close(fd);
		}
		if (!ok) {
			wlr_log(WLR_ERROR, "gl: no fence for a frame");
		}
	} else {
		fglFlush();
	}
	fglBindFramebuffer(GL_FRAMEBUFFER, 0);
	return ok;
}
