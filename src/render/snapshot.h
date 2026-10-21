#ifndef FLAME_RENDER_SNAPSHOT_H
#define FLAME_RENDER_SNAPSHOT_H

#include <wlr/util/box.h>

struct flame_frame;
struct flame_node;
struct wlr_texture;

// node and everything under it as frame's output shows it in box, into a
// texture: box (layout coords) comes back cut to the output, src is where
// in the texture it is. NULL when there's nothing to draw it with. the
// caller destroys it
struct wlr_texture *render_snapshot(struct flame_frame *frame, struct flame_node *node,
	struct wlr_box *box, struct wlr_fbox *src);
// get the buffers snapshots go in ready ahead of time (an output came up or
// changed size): no close pays for setting one up
void render_snapshot_warm(struct flame_frame *frame);

struct flame_element_list;
struct flame_render;
struct wlr_buffer;
// list (collected for area, layout coords) drawn into buffer at scale, all of
// buffer cleared first: a window captured on its own (screen sharing one
// window). false when the gpu said no
bool render_elements_to_buffer(struct flame_render *render,
	const struct flame_element_list *list, struct wlr_box area, float scale,
	struct wlr_buffer *buffer);

#endif
