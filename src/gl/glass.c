#include <math.h>
#include <wlr/util/log.h>

#include "gl/gl.h"
#include "gl/internal.h"

// glass: the part of the buffer it reads (its box and reach around it)
// copied out, blurred by dual kawase (halved level by level, then grown
// back, a few taps each way: a wide blur for a few cheap passes), then the
// pane drawn from that over its box. see glass_src in gl.c

int gl_glass_reach(float blur, float refraction) {
	// a kawase blur spreads about twice what it's asked for before it fades
	return (int)ceilf(2 * fmaxf(blur, 0) + fmaxf(refraction, 0)) + 1;
}

// how many levels down and how far apart the taps: the blur spreads about
// offset * 2^passes. more passes over a wider offset, a wide offset alone
// leaves a grid in it
static int blur_passes(float blur, float *offset) {
	if (blur < 1) {
		*offset = 0;
		return 0;
	}
	int passes = 1;
	while (passes < GL_BLUR_LEVELS - 1 && (float)(2 << passes) * 1.5f <= blur) {
		passes++;
	}
	*offset = fminf(fmaxf(blur / (float)(1 << passes), 1), 4);
	return passes;
}

static void texture_params(void) {
	fglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	fglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	fglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	fglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

// the copy at least width x height, grown only
static void copy_ready(struct gl_blur *blur, int width, int height) {
	if (blur->copy && blur->copy_width >= width && blur->copy_height >= height) {
		return;
	}
	blur->copy_width = width > blur->copy_width ? width : blur->copy_width;
	blur->copy_height = height > blur->copy_height ? height : blur->copy_height;
	if (!blur->copy) {
		fglGenTextures(1, &blur->copy);
	}
	fglBindTexture(GL_TEXTURE_2D, blur->copy);
	fglTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, blur->copy_width, blur->copy_height, 0, GL_RGB,
		GL_UNSIGNED_BYTE, NULL);
	texture_params();
	fglBindTexture(GL_TEXTURE_2D, 0);
}

// level i at least width x height, its texture and framebuffer. grown
// only, a smaller glass uses the corner of a bigger one's
static bool level_ready(struct gl_blur *blur, int i, int width, int height) {
	if (blur->texture[i] && blur->width[i] >= width && blur->height[i] >= height) {
		return true;
	}
	int w = width > blur->width[i] ? width : blur->width[i];
	int h = height > blur->height[i] ? height : blur->height[i];
	if (!blur->texture[i]) {
		fglGenTextures(1, &blur->texture[i]);
		fglGenFramebuffers(1, &blur->fbo[i]);
	}
	fglBindTexture(GL_TEXTURE_2D, blur->texture[i]);
	fglTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	texture_params();
	fglBindFramebuffer(GL_FRAMEBUFFER, blur->fbo[i]);
	fglFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
		blur->texture[i], 0);
	bool ok = fglCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
	fglBindTexture(GL_TEXTURE_2D, 0);
	if (!ok) {
		wlr_log(WLR_ERROR, "gl: no framebuffer for a blur level %dx%d", w, h);
		blur->width[i] = blur->height[i] = 0;
		return false;
	}
	blur->width[i] = w;
	blur->height[i] = h;
	return true;
}

void gl_blur_finish(struct gl_blur *blur) {
	for (int i = 0; i < GL_BLUR_LEVELS; i++) {
		if (blur->texture[i]) {
			fglDeleteTextures(1, &blur->texture[i]);
			fglDeleteFramebuffers(1, &blur->fbo[i]);
		}
		blur->texture[i] = blur->fbo[i] = 0;
		blur->width[i] = blur->height[i] = 0;
	}
	if (blur->copy) {
		fglDeleteTextures(1, &blur->copy);
	}
	blur->copy = 0;
	blur->copy_width = blur->copy_height = 0;
}

// a texture, the part of it in use (from x, y) and how big it's allocated
struct level {
	unsigned texture;
	int x, y, width, height, alloc_width, alloc_height;
};

static struct level level_of(const struct gl_blur *blur, int i, int width, int height) {
	return (struct level){blur->texture[i], 0, 0, width, height, blur->width[i],
		blur->height[i]};
}

// the used part of a level in its texture's coords, half a texel in:
// nothing outside it gets sampled
static void set_bounds(const struct gl_program *program, struct level level) {
	float tw = (float)level.alloc_width, th = (float)level.alloc_height;
	fglUniform4f(program->bounds, (level.x + 0.5f) / tw, (level.y + 0.5f) / th,
		(level.x + level.width - 0.5f) / tw, (level.y + level.height - 0.5f) / th);
}

// one level into the next, down (bigger to smaller) or up
static void blur_step(const struct gl_program *program, struct level from, unsigned to_fbo,
		int to_w, int to_h, float offset) {
	fglBindFramebuffer(GL_FRAMEBUFFER, to_fbo);
	fglViewport(0, 0, to_w, to_h);
	fglUseProgram(program->program);
	fglBindTexture(GL_TEXTURE_2D, from.texture);
	fglUniform1i(program->tex, 0);
	fglUniform2f(program->texel, 1.0f / from.alloc_width, 1.0f / from.alloc_height);
	fglUniform1f(program->offset, offset);
	set_bounds(program, from);
	float proj[9];
	gl_target_proj(proj, to_w, to_h);
	gl_box_proj(proj, program->proj, (struct wlr_box){0, 0, to_w, to_h});
	gl_tex_proj(program->tex_proj, WL_OUTPUT_TRANSFORM_NORMAL, (struct wlr_fbox){
		(double)from.x / from.alloc_width, (double)from.y / from.alloc_height,
		(double)from.width / from.alloc_width, (double)from.height / from.alloc_height});
	gl_draw_box((struct wlr_box){0, 0, to_w, to_h}, NULL, program->pos);
}

void gl_pass_glass(struct gl_pass *pass, const struct gl_glass_options *options) {
	struct flame_gl *gl = pass->gl;
	struct wlr_box box = options->box;
	if (wlr_box_empty(&box) || options->alpha <= 0) {
		return;
	}
	pixman_region32_t shown;
	pixman_region32_init_rect(&shown, box.x, box.y, box.width, box.height);
	if (options->clip) {
		pixman_region32_intersect(&shown, &shown, options->clip);
	}
	bool any = pixman_region32_not_empty(&shown);
	pixman_region32_fini(&shown);
	if (!any) {
		return;
	}

	// what it reads, cut to the buffer
	int grow = gl_glass_reach(options->blur, options->refraction);
	struct wlr_box reach = {box.x - grow, box.y - grow, box.width + 2 * grow,
		box.height + 2 * grow};
	struct wlr_box buffer = {0, 0, pass->width, pass->height};
	if (!wlr_box_intersection(&reach, &reach, &buffer)) {
		return;
	}
	float offset;
	int passes = blur_passes(options->blur, &offset);
	int w[GL_BLUR_LEVELS], h[GL_BLUR_LEVELS];
	w[0] = reach.width;
	h[0] = reach.height;
	for (int i = 1; i <= passes; i++) {
		w[i] = w[i - 1] > 1 ? w[i - 1] / 2 : 1;
		h[i] = h[i - 1] > 1 ? h[i - 1] / 2 : 1;
	}
	struct gl_blur *blur = &gl->blur;
	for (int i = 0; i <= passes; i++) {
		if (passes && !level_ready(blur, i, w[i], h[i])) {
			fglBindFramebuffer(GL_FRAMEBUFFER, pass->fbo);
			return;
		}
	}
	// what's behind it: the backdrop kept, or as drawn so far, copied out with
	// the top row of reach as row 0
	struct level behind = {blur->copy, 0, 0, w[0], h[0], 0, 0};
	struct wlr_box kept;
	if (wlr_box_intersection(&kept, &pass->backdrop, &reach) && wlr_box_equal(&kept, &reach)) {
		behind.x = reach.x - pass->backdrop.x;
		behind.y = reach.y - pass->backdrop.y;
	} else {
		// it reads past what's kept: the copy's about to be another one
		pass->backdrop = (struct wlr_box){0};
		copy_ready(blur, w[0], h[0]);
		fglBindFramebuffer(GL_FRAMEBUFFER, pass->fbo);
		fglActiveTexture(GL_TEXTURE0);
		fglBindTexture(GL_TEXTURE_2D, blur->copy);
		fglCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, reach.x, reach.y, reach.width,
			reach.height);
	}
	behind.alloc_width = blur->copy_width;
	behind.alloc_height = blur->copy_height;

	// down from the copy, back up into level 0
	fglDisable(GL_BLEND);
	for (int i = 0; i < passes; i++) {
		struct level from = i == 0 ? behind : level_of(blur, i, w[i], h[i]);
		blur_step(&gl->blur_down, from, blur->fbo[i + 1], w[i + 1], h[i + 1], offset);
	}
	for (int i = passes; i > 0; i--) {
		blur_step(&gl->blur_up, level_of(blur, i, w[i], h[i]), blur->fbo[i - 1], w[i - 1],
			h[i - 1], offset);
	}
	struct level blurred = passes ? level_of(blur, 0, w[0], h[0]) : behind;

	// the pane over the buffer again
	fglBindFramebuffer(GL_FRAMEBUFFER, pass->fbo);
	fglViewport(0, 0, pass->width, pass->height);
	fglEnable(GL_BLEND);
	const struct gl_program *program = &gl->glass;
	fglUseProgram(program->program);
	fglBindTexture(GL_TEXTURE_2D, blurred.texture);
	fglUniform1i(program->tex, 0);
	fglUniform4f(program->reach, reach.x - blurred.x, reach.y - blurred.y, blurred.alloc_width,
		blurred.alloc_height);
	set_bounds(program, blurred);
	fglUniform4f(program->box, box.x, box.y, box.width, box.height);
	fglUniform1f(program->radius, options->radius);
	fglUniform4f(program->tint, options->tint[0], options->tint[1], options->tint[2],
		options->tint[3]);
	fglUniform1f(program->saturation, options->saturation);
	fglUniform1f(program->edge, options->edge);
	fglUniform1f(program->refraction, options->refraction);
	fglUniform1f(program->rim, fmaxf(options->rim, 0.5f));
	fglUniform1f(program->alpha, options->alpha);
	gl_box_proj(pass->proj, program->proj, box);
	// v_texcoord comes out as the buffer pixel
	gl_tex_proj(program->tex_proj, WL_OUTPUT_TRANSFORM_NORMAL,
		(struct wlr_fbox){box.x, box.y, box.width, box.height});
	gl_draw_box(box, options->clip, program->pos);
	fglBindTexture(GL_TEXTURE_2D, 0);
}

void gl_pass_backdrop(struct gl_pass *pass, struct wlr_box area) {
	pass->backdrop = (struct wlr_box){0};
	struct wlr_box buffer = {0, 0, pass->width, pass->height};
	if (!pass->gl || !wlr_box_intersection(&area, &area, &buffer)) {
		return;
	}
	struct gl_blur *blur = &pass->gl->blur;
	copy_ready(blur, area.width, area.height);
	fglBindFramebuffer(GL_FRAMEBUFFER, pass->fbo);
	fglActiveTexture(GL_TEXTURE0);
	fglBindTexture(GL_TEXTURE_2D, blur->copy);
	fglCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, area.x, area.y, area.width, area.height);
	fglBindTexture(GL_TEXTURE_2D, 0);
	pass->backdrop = area;
}

void gl_pass_backdrop_end(struct gl_pass *pass) {
	pass->backdrop = (struct wlr_box){0};
}
