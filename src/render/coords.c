#include "render/coords.h"

#include <math.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/region.h>
#include <wlr/util/transform.h>

void target_init(struct flame_render_target *target, struct wlr_output *output,
		struct wlr_box logical) {
	target->logical = logical;
	target->scale = output->scale;
	target->transform = output->transform;
	target->trans_width = output->width;
	target->trans_height = output->height;
	wlr_output_transform_coords(output->transform, &target->trans_width,
		&target->trans_height);
}

void target_region_to_buffer(const struct flame_render_target *target,
		pixman_region32_t *region, bool round_up) {
	pixman_region32_translate(region, -target->logical.x, -target->logical.y);
	wlr_region_scale(region, region, target->scale);
	if (round_up && floor(target->scale) != target->scale) {
		// a partly covered pixel still has to be redrawn
		wlr_region_expand(region, region, 1);
	}
	wlr_region_transform(region, region, wlr_output_transform_invert(target->transform),
		target->trans_width, target->trans_height);
}

// rounding both ends instead of the size keeps neighbours from gapping or
// overlapping at fractional scales
static int scale_length(int length, int offset, float scale) {
	return (int)(round((offset + length) * scale) - round(offset * scale));
}

void target_box_to_buffer(const struct flame_render_target *target, struct wlr_box *box) {
	box->x -= target->logical.x;
	box->y -= target->logical.y;
	box->width = scale_length(box->width, box->x, target->scale);
	box->height = scale_length(box->height, box->y, target->scale);
	box->x = (int)round(box->x * target->scale);
	box->y = (int)round(box->y * target->scale);
	wlr_box_transform(box, box, wlr_output_transform_invert(target->transform),
		target->trans_width, target->trans_height);
}
