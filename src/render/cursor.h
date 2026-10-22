#ifndef FLAME_RENDER_CURSOR_H
#define FLAME_RENDER_CURSOR_H

#include <stdbool.h>
#include <wlr/util/box.h>

#include "cursor/plane.h"

struct flame_element_list;
struct flame_render;
struct flame_render_target;
struct wlr_output;
struct wlr_surface;
struct wlr_texture;
struct wlr_xcursor_image;

// the pointer on one output, decided per frame: on the cursor plane when
// the image fits there, KMS moves it and nothing gets composited. otherwise
// it's the frame's topmost element, drawn like anything else
struct flame_frame_cursor {
	struct flame_render *render;
	struct wlr_output *output;
	struct flame_cursor_plane plane;

	// the theme's or X's cursor as a texture, for drawing it ourselves
	struct wlr_texture *texture;
	const struct wlr_xcursor_image *texture_image;
	uint32_t texture_generation;

	// a client's cursor surface on the plane: it's not in the element list
	// but still gets enter and frame callbacks, see frame_cursor_surface
	struct wlr_surface *plane_surface;
};

void frame_cursor_init(struct flame_frame_cursor *cursor, struct flame_render *render,
	struct wlr_output *output);
void frame_cursor_finish(struct flame_frame_cursor *cursor);

// this frame's pointer: on the plane, or pushed onto list. false when the
// list ran out of memory
bool frame_cursor_build(struct flame_frame_cursor *cursor,
	const struct flame_render_target *target, struct flame_element_list *list);
// the pointer pushed onto list as it looks on this output, plane or not: a
// screen being shared shows it. false when the list ran out of memory
bool frame_cursor_push(struct flame_frame_cursor *cursor, struct wlr_box area,
	struct flame_element_list *list);
// the client cursor surface on the plane right now, NULL for none
struct wlr_surface *frame_cursor_surface(struct flame_frame_cursor *cursor);
// the plane changed and needs a commit to show it
bool frame_cursor_dirty(const struct flame_frame_cursor *cursor);
void frame_cursor_committed(struct flame_frame_cursor *cursor);
// back from a vt switch, KMS needs the image again
void frame_cursor_reset(struct flame_frame_cursor *cursor);

#endif
