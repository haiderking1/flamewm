#ifndef FLAME_RENDER_SNAPSHOT_H
#define FLAME_RENDER_SNAPSHOT_H

#include <wlr/util/box.h>

struct flame_node;
struct flame_render;
struct wlr_output;
struct wlr_texture;

// node and everything under it, drawn into a texture box big, box starting
// at the node's position, in a format output's buffers use. NULL when
// there's nothing to draw it with. the caller destroys it
struct wlr_texture *render_snapshot(struct flame_render *render, struct wlr_output *output,
	struct flame_node *node, struct wlr_box box);

#endif
