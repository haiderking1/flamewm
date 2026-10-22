#ifndef FLAME_RENDER_TEXT_H
#define FLAME_RENDER_TEXT_H

struct wlr_renderer;
struct wlr_texture;

// a line of text as a texture: font a pango description ("sans 11"), cut
// with an ellipsis to max_width layout units, drawn at scale so it's sharp.
// width, height: its layout size. NULL for an empty line or no memory. the
// caller destroys it
struct wlr_texture *render_text(struct wlr_renderer *renderer, const char *text,
	const char *font, const float color[4], int max_width, float scale, int *width,
	int *height);

#endif
