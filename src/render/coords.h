#ifndef FLAME_RENDER_COORDS_H
#define FLAME_RENDER_COORDS_H

#include <pixman.h>
#include <wayland-server-protocol.h>
#include <wlr/util/box.h>

struct wlr_output;

// how layout coords land in an output's buffer: shifted to the output,
// scaled, then turned by its transform
struct flame_render_target {
	struct wlr_box logical; // the output in layout coords
	float scale;
	enum wl_output_transform transform;
	int trans_width, trans_height; // buffer size before the transform
};

void target_init(struct flame_render_target *target, struct wlr_output *output,
	struct wlr_box logical);
// round_up grows regions to whole pixels at fractional scales, for damage
void target_region_to_buffer(const struct flame_render_target *target,
	pixman_region32_t *region, bool round_up);
void target_box_to_buffer(const struct flame_render_target *target, struct wlr_box *box);

#endif
