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
	// the radius from levels down, not from samples far apart: past about a
	// texel and a half they skip what's between them, and it comes out in
	// blocks and bands. another level halves it again
	int passes = 1;
	while (passes < GL_BLUR_LEVELS - 1 && blur / (float)(1 << passes) > 1.5f) {
		passes++;
	}
	*offset = fminf(fmaxf(blur / (float)(1 << passes), 0.75f), 1.5f);
	return passes;
}

static void texture_params(void) {
	fglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	fglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	fglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	fglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

// a texture's side grown to fit needed, with room to spare: glass that
// grows a little every frame (opening, animated) isn't a new texture every
// frame
static int roomy(int needed, int have) {
	if (needed <= have) {
		return have;
	}
	needed += needed / 4;
	return (needed + 127) & ~127;
}

// the copy at least width x height, grown only
static void copy_ready(struct gl_blur *blur, int width, int height) {
	if (blur->copy && blur->copy_width >= width && blur->copy_height >= height) {
		return;
	}
	blur->copy_width = roomy(width, blur->copy_width);
	blur->copy_height = roomy(height, blur->copy_height);
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
	int w = roomy(width, blur->width[i]);
	int h = roomy(height, blur->height[i]);
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

// the copy as a framebuffer the blit writes into, made the first time. false:
// it can't be one (rgb not renderable here), the blit's no use then
static bool copy_target(struct flame_gl *gl) {
	struct gl_blur *blur = &gl->blur;
	if (!blur->copy_fbo) {
		fglGenFramebuffers(1, &blur->copy_fbo);
	}
	fglBindFramebuffer(GL_FRAMEBUFFER, blur->copy_fbo);
	fglFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, blur->copy, 0);
	if (fglCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		wlr_log(WLR_INFO, "gl: the glass copy can't be blitted into, full size copies");
		gl->blit = false;
		return false;
	}
	return true;
}

// area (even) blitted from the buffer into the copy at half size, filtered.
// false: it couldn't be
static bool blit_half(struct gl_pass *pass, struct wlr_box area) {
	struct flame_gl *gl = pass->gl;
	struct gl_blur *blur = &gl->blur;
	copy_ready(blur, area.width / 2, area.height / 2);
	if (!copy_target(gl)) {
		fglBindFramebuffer(GL_FRAMEBUFFER, pass->fbo);
		return false;
	}
	fglBindFramebuffer(GL_READ_FRAMEBUFFER_NV, pass->fbo);
	fglBindFramebuffer(GL_DRAW_FRAMEBUFFER_NV, blur->copy_fbo);
	fglBlitFramebuffer(area.x, area.y, area.x + area.width, area.y + area.height, 0, 0,
		area.width / 2, area.height / 2, GL_COLOR_BUFFER_BIT, GL_LINEAR);
	fglBindFramebuffer(GL_FRAMEBUFFER, pass->fbo);
	gl->stats.copy += (uint64_t)(area.width / 2) * (uint64_t)(area.height / 2);
	return true;
}

// box grown out to even coordinates (a half size copy's pixels then stand
// for whole pairs), still inside the buffer
static struct wlr_box even(struct wlr_box box, int width, int height) {
	int x1 = box.x & ~1, y1 = box.y & ~1;
	int x2 = (box.x + box.width + 1) & ~1, y2 = (box.y + box.height + 1) & ~1;
	x2 = x2 > (width & ~1) ? (width & ~1) : x2;
	y2 = y2 > (height & ~1) ? (height & ~1) : y2;
	return (struct wlr_box){x1, y1, x2 - x1, y2 - y1};
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
	if (blur->copy_fbo) {
		fglDeleteFramebuffers(1, &blur->copy_fbo);
	}
	blur->copy = blur->copy_fbo = 0;
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
static void blur_step(struct flame_gl *gl, const struct gl_program *program, struct level from,
		unsigned to_fbo,
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
	gl->stats.blur += gl_draw_box((struct wlr_box){0, 0, to_w, to_h}, NULL, program->pos);
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
	struct gl_blur *blur = &gl->blur;
	// half: what's behind comes in at half size, already level 1 (a blit,
	// filtered), when it blurs at all and nothing's kept at full size for it
	struct wlr_box kept;
	bool backdrop = !wlr_box_empty(&pass->backdrop);
	bool half = gl->blit && passes && (!backdrop || pass->backdrop_half);
	if (half) {
		reach = even(reach, pass->width, pass->height);
		if (wlr_box_empty(&reach)) {
			return;
		}
	}
	int w[GL_BLUR_LEVELS], h[GL_BLUR_LEVELS];
	w[0] = reach.width;
	h[0] = reach.height;
	for (int i = 1; i <= passes; i++) {
		w[i] = w[i - 1] > 1 ? w[i - 1] / 2 : 1;
		h[i] = h[i - 1] > 1 ? h[i - 1] / 2 : 1;
	}
	// the levels the blur writes: down from level 1 (from the copy, half
	// size) or level 0 (the copy, full size), and back up to level 1
	for (int i = half ? 2 : 1; i <= passes; i++) {
		if (!level_ready(blur, i, w[i], h[i])) {
			fglBindFramebuffer(GL_FRAMEBUFFER, pass->fbo);
			return;
		}
	}
	if (half && passes > 1 && !level_ready(blur, 1, w[1], h[1])) {
		fglBindFramebuffer(GL_FRAMEBUFFER, pass->fbo);
		return;
	}
	// what's behind it, the top row of reach as row 0: the backdrop kept, or
	// as drawn so far copied out. half size: that's level 1 already
	int scale = half ? 2 : 1;
	struct level behind = {blur->copy, 0, 0, w[half], h[half], 0, 0};
	if (backdrop && wlr_box_intersection(&kept, &pass->backdrop, &reach)
			&& wlr_box_equal(&kept, &reach)) {
		behind.x = (reach.x - pass->backdrop.x) / scale;
		behind.y = (reach.y - pass->backdrop.y) / scale;
	} else if (half) {
		pass->backdrop = (struct wlr_box){0};
		if (!blit_half(pass, reach)) {
			return; // next frame copies full size
		}
	} else {
		// it reads past what's kept: the copy's about to be another one
		pass->backdrop = (struct wlr_box){0};
		copy_ready(blur, w[0], h[0]);
		fglBindFramebuffer(GL_FRAMEBUFFER, pass->fbo);
		fglActiveTexture(GL_TEXTURE0);
		fglBindTexture(GL_TEXTURE_2D, blur->copy);
		fglCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, reach.x, reach.y, reach.width,
			reach.height);
		gl->stats.copy += (uint64_t)reach.width * (uint64_t)reach.height;
	}
	behind.alloc_width = blur->copy_width;
	behind.alloc_height = blur->copy_height;

	// down from what's behind, back up to level 1: the last step up the pane
	// does itself as it's drawn, no full size picture written and read again
	fglDisable(GL_BLEND);
	int first = half ? 1 : 0; // the level behind is
	if (passes) {
		gl->stats.chains++;
		gl->stats.steps += (uint64_t)(passes - first) * 2 - (half ? 0 : 1);
	}
	for (int i = first; i < passes; i++) {
		struct level from = i == first ? behind : level_of(blur, i, w[i], h[i]);
		blur_step(gl, &gl->blur_down, from, blur->fbo[i + 1], w[i + 1], h[i + 1], offset);
	}
	for (int i = passes; i > 1; i--) {
		blur_step(gl, &gl->blur_up, level_of(blur, i, w[i], h[i]), blur->fbo[i - 1], w[i - 1],
			h[i - 1], offset);
	}
	// level 1: written by the way up, or (one level, half size) the copy as is
	struct level blurred = !passes ? behind
		: half && passes == 1 ? behind : level_of(blur, 1, w[1], h[1]);

	// the pane over the buffer again
	fglBindFramebuffer(GL_FRAMEBUFFER, pass->fbo);
	fglViewport(0, 0, pass->width, pass->height);
	fglEnable(GL_BLEND);
	const struct gl_program *program = &gl->glass;
	fglUseProgram(program->program);
	fglBindTexture(GL_TEXTURE_2D, blurred.texture);
	fglUniform1i(program->tex, 0);
	// the buffer pixels the texture's allocation spans: 1:1 for the copy, a
	// level down as many as its pixels stand for (w[0] / w[1], odd sizes too)
	float span_x = passes ? (float)blurred.alloc_width * w[0] / w[1] : blurred.alloc_width;
	float span_y = passes ? (float)blurred.alloc_height * h[0] / h[1] : blurred.alloc_height;
	// where the texture starts in the buffer: its used part is reach's
	float origin_x = reach.x - blurred.x * (span_x / blurred.alloc_width);
	float origin_y = reach.y - blurred.y * (span_y / blurred.alloc_height);
	fglUniform4f(program->reach, origin_x, origin_y, span_x, span_y);
	set_bounds(program, blurred);
	fglUniform2f(program->texel, 1.0f / blurred.alloc_width, 1.0f / blurred.alloc_height);
	fglUniform1f(program->offset, passes ? offset : 0.0f);
	fglUniform4f(program->box, box.x, box.y, box.width, box.height);
	fglUniform1f(program->radius, options->radius);
	fglUniform4f(program->tint, options->tint[0], options->tint[1], options->tint[2],
		options->tint[3]);
	fglUniform1f(program->saturation, options->saturation);
	fglUniform1f(program->edge, options->edge);
	fglUniform1f(program->refraction, options->refraction);
	fglUniform1f(program->rim, fmaxf(options->rim, 0.5f));
	fglUniform1f(program->alpha, options->alpha);
	fglUniform1f(program->noise, pass->noise);
	gl_box_proj(pass->proj, program->proj, box);
	// v_texcoord comes out as the buffer pixel
	gl_tex_proj(program->tex_proj, WL_OUTPUT_TRANSFORM_NORMAL,
		(struct wlr_fbox){box.x, box.y, box.width, box.height});
	gl->stats.glass += gl_draw_box(box, options->clip, program->pos);
	fglBindTexture(GL_TEXTURE_2D, 0);
}

void gl_pass_backdrop(struct gl_pass *pass, struct wlr_box area, bool blurred) {
	pass->backdrop = (struct wlr_box){0};
	pass->backdrop_half = false;
	struct wlr_box buffer = {0, 0, pass->width, pass->height};
	if (!pass->gl || !wlr_box_intersection(&area, &area, &buffer)) {
		return;
	}
	if (blurred && pass->gl->blit) {
		// every pane blurs: kept at half size, each pane's level 1 is its part
		area = even(area, pass->width, pass->height);
		if (!wlr_box_empty(&area) && blit_half(pass, area)) {
			pass->backdrop = area;
			pass->backdrop_half = true;
			return;
		}
	}
	struct gl_blur *blur = &pass->gl->blur;
	copy_ready(blur, area.width, area.height);
	fglBindFramebuffer(GL_FRAMEBUFFER, pass->fbo);
	fglActiveTexture(GL_TEXTURE0);
	fglBindTexture(GL_TEXTURE_2D, blur->copy);
	fglCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, area.x, area.y, area.width, area.height);
	pass->gl->stats.copy += (uint64_t)area.width * (uint64_t)area.height;
	fglBindTexture(GL_TEXTURE_2D, 0);
	pass->backdrop = area;
}

void gl_pass_backdrop_end(struct gl_pass *pass) {
	pass->backdrop = (struct wlr_box){0};
	pass->backdrop_half = false;
}
