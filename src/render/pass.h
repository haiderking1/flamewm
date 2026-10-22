#ifndef FLAME_RENDER_PASS_H
#define FLAME_RENDER_PASS_H

#include <pixman.h>
#include <stdbool.h>
#include <stdint.h>

#include "gl/gl.h"

struct flame_render;
struct wlr_buffer;
struct wlr_drm_syncobj_timeline;
struct wlr_render_pass;

// one buffer being drawn, on the caller's stack: by flame's own renderer
// (gl/), or by wlroots' render pass where there's no GL to draw with
// (pixman, vulkan, FLAME_RENDERER=wlroots). the same picture either way,
// what only flame's draws (effects) falls back to something plainer
struct flame_pass {
	struct gl_pass gl;
	struct wlr_render_pass *wlr; // NULL: ours
};

bool pass_begin(struct flame_pass *pass, struct flame_render *render,
	struct wlr_buffer *buffer, struct wlr_drm_syncobj_timeline *signal, uint64_t point);
// buffer coords, color premultiplied. blend off: it replaces what's there
void pass_rect(struct flame_pass *pass, struct wlr_box box, const float color[4],
	const pixman_region32_t *clip, bool blend);
void pass_texture(struct flame_pass *pass, const struct gl_texture_options *options);
// frosted glass over what's drawn so far (see gl_pass_glass), only its tint
// without flame's renderer
void pass_glass(struct flame_pass *pass, const struct gl_glass_options *options);
// a scene glass's look at an output's scale, faded to alpha
struct flame_glass_style;
void pass_glass_style(struct flame_pass *pass, const struct flame_glass_style *style,
	struct wlr_box box, const pixman_region32_t *clip, float scale, float alpha);
bool pass_submit(struct flame_pass *pass);

#endif
