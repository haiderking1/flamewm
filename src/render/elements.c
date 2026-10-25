#include "render/elements.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_compositor.h>
#include <drm_fourcc.h>
#include <wlr/util/log.h>

#include "eyecandy/eyecandy.h"
#include "scene/scene.h"

void elements_init(struct flame_element_list *list) {
	*list = (struct flame_element_list){0};
}

void elements_clear(struct flame_element_list *list) {
	for (size_t i = 0; i < list->len; i++) {
		pixman_region32_fini(&list->data[i].visible);
		pixman_region32_fini(&list->data[i].opaque);
	}
	list->len = 0;
}

void elements_forget_surface(struct flame_element_list *list, struct wlr_surface *surface) {
	for (size_t i = 0; i < list->len; i++) {
		struct flame_element *element = &list->data[i];
		if (element->type == FLAME_ELEMENT_SURFACE && element->surface == surface) {
			element->type = FLAME_ELEMENT_GONE;
			element->surface = NULL;
			element->texture = NULL;
			element->buffer = NULL;
		}
	}
}

void elements_finish(struct flame_element_list *list) {
	elements_clear(list);
	free(list->data);
	elements_init(list);
}

static struct flame_element *push(struct flame_element_list *list) {
	if (list->len == list->cap) {
		size_t cap = list->cap ? list->cap * 2 : 64;
		struct flame_element *data = realloc(list->data, cap * sizeof(*data));
		if (!data) {
			return NULL;
		}
		list->data = data;
		list->cap = cap;
	}
	struct flame_element *element = &list->data[list->len++];
	memset(element, 0, sizeof(*element));
	element->alpha = 1.0f;
	pixman_region32_init(&element->visible);
	pixman_region32_init(&element->opaque);
	return element;
}

// where things land: x' = ax * x + bx, y' = ay * y + by, faded to alpha.
// the identity unless an effect is on the way down
struct transform {
	double ax, bx, ay, by;
	float alpha;
};

// faded or scaled: see through, or its opaque region doesn't fit its box.
// only moved (a workspace sliding in) it's as solid as ever, just elsewhere:
// what's behind it still needn't be drawn
static bool faded_or_scaled(const struct transform *t) {
	return t->ax != 1 || t->ay != 1 || t->alpha < 1;
}

static struct wlr_box apply(const struct transform *t, struct wlr_box box) {
	if (t->ax == 1 && t->ay == 1 && t->bx == 0 && t->by == 0) {
		return box;
	}
	double x0 = t->ax * box.x + t->bx, x1 = t->ax * (box.x + box.width) + t->bx;
	double y0 = t->ay * box.y + t->by, y1 = t->ay * (box.y + box.height) + t->by;
	int left = (int)(x0 + 0.5), top = (int)(y0 + 0.5);
	return (struct wlr_box){left, top, (int)(x1 + 0.5) - left, (int)(y1 + 0.5) - top};
}

struct collect {
	struct flame_element_list *list;
	struct wlr_box area;
	// the surface node being flattened
	int x, y;
	const struct wlr_box *clip; // layout coords before the transform, NULL for none
	struct transform t;
	// a snapshot: surfaces just unmapped still have their last buffer
	bool unmapped_too;
	bool failed;
	// the rounded window being flattened (transformed), a radius of 0 for none
	struct wlr_box shape;
	float shape_radius;
};

static void mark(struct flame_element *element, const struct collect *collect) {
	element->alpha = collect->t.alpha;
	element->effected = faded_or_scaled(&collect->t);
}

// box cut down to what can show on this output, false when nothing can.
// box and clip are already transformed
static bool clip_box(const struct collect *collect, struct wlr_box box,
		const struct wlr_box *clip, struct wlr_box *out) {
	if (!wlr_box_intersection(out, &box, &collect->area)) {
		return false;
	}
	return !clip || wlr_box_intersection(out, out, clip);
}

// box, surface local and any size an app sends, cut down to a width x
// height surface. false when none of it's on it
static bool clip_local(struct wlr_box box, int width, int height, struct wlr_box *out) {
	int64_t x1 = box.x > 0 ? box.x : 0, y1 = box.y > 0 ? box.y : 0;
	int64_t x2 = (int64_t)box.x + box.width, y2 = (int64_t)box.y + box.height;
	x2 = x2 < width ? x2 : width;
	y2 = y2 < height ? y2 : height;
	if (x2 <= x1 || y2 <= y1) {
		return false;
	}
	*out = (struct wlr_box){(int)x1, (int)y1, (int)(x2 - x1), (int)(y2 - y1)};
	return true;
}

// a 1x1 buffer whose pixel has no alpha: a surface that's there and shows
// nothing (a single-pixel buffer of no colour)
static bool clear_pixel(struct wlr_surface *surface) {
	struct wlr_buffer *buffer = surface->buffer ? surface->buffer->source : NULL;
	if (!buffer || buffer->width != 1 || buffer->height != 1) {
		return false;
	}
	void *data;
	uint32_t format;
	size_t stride;
	if (!wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ, &data,
			&format, &stride)) {
		return false;
	}
	// alpha's the last byte of either
	bool clear = (format == DRM_FORMAT_ARGB8888 || format == DRM_FORMAT_ABGR8888)
		&& ((const uint8_t *)data)[3] == 0;
	wlr_buffer_end_data_ptr_access(buffer);
	return clear;
}

static void collect_surface(struct wlr_surface *surface, int sx, int sy, void *data) {
	struct collect *collect = data;
	struct wlr_texture *texture = wlr_surface_get_texture(surface);
	if (!texture || collect->failed) {
		return;
	}
	struct wlr_box box = apply(&collect->t, (struct wlr_box){
		.x = collect->x + sx,
		.y = collect->y + sy,
		.width = surface->current.width,
		.height = surface->current.height,
	});
	struct wlr_box node_clip = collect->clip ? apply(&collect->t, *collect->clip)
		: (struct wlr_box){0};
	struct wlr_box clip;
	if (wlr_box_empty(&box) || !clip_box(collect, box, collect->clip ? &node_clip : NULL,
			&clip)) {
		return;
	}

	// its glass first, right under it: what's behind it, frosted
	size_t panes_len;
	const struct eyecandy_pane *panes = eyecandy_panes(surface, &panes_len);
	// faded all together (set_opacity), all the way: none of them
	float opacity = (float)eyecandy_opacity(surface);
	for (size_t i = 0; opacity > 0 && i < panes_len; i++) {
		if (panes[i].opacity <= 0) {
			continue; // faded all the way on its own
		}
		// clipped to the surface in its own coords first: "all of it" comes as
		// 0, 0, INT32_MAX, INT32_MAX (foot's blur), moved before it's cut
		// down it would overflow
		struct wlr_box pane_box;
		if (!clip_local(panes[i].box, surface->current.width, surface->current.height,
				&pane_box)) {
			continue;
		}
		pane_box.x += collect->x + sx;
		pane_box.y += collect->y + sy;
		pane_box = apply(&collect->t, pane_box);
		struct wlr_box pane_clip;
		if (wlr_box_empty(&pane_box) || !clip_box(collect, pane_box,
				collect->clip ? &node_clip : NULL, &pane_clip)) {
			continue;
		}
		struct flame_element *pane = push(collect->list);
		if (!pane) {
			collect->failed = true;
			return;
		}
		pane->type = FLAME_ELEMENT_GLASS;
		pane->id = &panes[i];
		pane->box = pane_box;
		pane->clip = pane_clip;
		pane->glass = panes[i].style;
		pane->glass_group = surface;
		pane->glass.radius *= collect->t.ax;
		pane->glass.blur *= collect->t.ax;
		pane->glass.refraction *= collect->t.ax;
		mark(pane, collect);
		pane->alpha *= opacity * panes[i].opacity;
	}

	struct flame_element *element = push(collect->list);
	if (!element) {
		collect->failed = true;
		return;
	}
	element->type = FLAME_ELEMENT_SURFACE;
	element->id = surface;
	element->box = box;
	element->clip = clip;
	element->shape = collect->shape;
	element->shape_radius = collect->shape_radius;
	element->surface = surface;
	element->texture = texture;
	wlr_surface_get_buffer_source_box(surface, &element->src);
	element->transform = surface->current.transform;
	element->buffer = surface->buffer ? &surface->buffer->base : NULL;
	element->clear = clear_pixel(surface);
	mark(element, collect);
}

static void collect_node(struct collect *collect, struct flame_node *node, int x, int y) {
	if (!node->enabled || collect->failed) {
		return;
	}
	x += node->x;
	y += node->y;

	switch (node->type) {
	case FLAME_NODE_TREE:;
		struct flame_tree *tree = scene_tree_from_node(node);
		struct transform outer = collect->t;
		if (tree->has_effect) {
			// p -> origin + (p - origin) * scale + d, then whatever's above
			const struct flame_effect *e = &tree->effect;
			double ox = x + e->origin_x, oy = y + e->origin_y;
			double cx = ox * (1 - e->scale_x) + e->dx, cy = oy * (1 - e->scale_y) + e->dy;
			collect->t.bx = outer.ax * cx + outer.bx;
			collect->t.by = outer.ay * cy + outer.by;
			collect->t.ax = outer.ax * e->scale_x;
			collect->t.ay = outer.ay * e->scale_y;
			collect->t.alpha = outer.alpha * e->opacity;
			if (collect->t.alpha <= 0.0f) {
				collect->t = outer;
				break; // all faded out, nothing to draw
			}
		}
		size_t from = collect->list->len;
		struct flame_node *child;
		wl_list_for_each(child, &tree->children, link) {
			collect_node(collect, child, x, y);
		}
		if (tree->under_windows) {
			for (size_t k = from; k < collect->list->len; k++) {
				collect->list->data[k].under_windows = true;
			}
		}
		collect->t = outer;
		break;
	case FLAME_NODE_RECT:;
		struct flame_rect *rect = wl_container_of(node, rect, node);
		struct wlr_box box = apply(&collect->t, (struct wlr_box){x, y, rect->width,
			rect->height});
		struct wlr_box clip;
		// premultiplied, so a clear rect is all zero
		if (rect->color[3] <= 0 || wlr_box_empty(&box)
				|| !clip_box(collect, box, NULL, &clip)) {
			break;
		}
		struct flame_element *element = push(collect->list);
		if (!element) {
			collect->failed = true;
			break;
		}
		element->type = FLAME_ELEMENT_RECT;
		element->id = rect;
		element->box = box;
		element->clip = clip;
		memcpy(element->color, rect->color, sizeof(element->color));
		mark(element, collect);
		break;
	case FLAME_NODE_GLASS:;
		struct flame_glass *glass = wl_container_of(node, glass, node);
		struct wlr_box glass_box = apply(&collect->t, (struct wlr_box){x, y, glass->width,
			glass->height});
		struct wlr_box glass_clip;
		if (wlr_box_empty(&glass_box) || !clip_box(collect, glass_box, NULL, &glass_clip)) {
			break;
		}
		struct flame_element *pane = push(collect->list);
		if (!pane) {
			collect->failed = true;
			break;
		}
		pane->type = FLAME_ELEMENT_GLASS;
		pane->id = glass;
		pane->box = glass_box;
		pane->clip = glass_clip;
		// scaled along with the box, a zoom scales its corners and blur too
		pane->glass = glass->style;
		pane->glass.radius *= collect->t.ax;
		pane->glass.blur *= collect->t.ax;
		pane->glass.refraction *= collect->t.ax;
		mark(pane, collect);
		break;
	case FLAME_NODE_SURFACE:;
		struct flame_surface_node *surface_node = wl_container_of(node, surface_node, node);
		if (!surface_node->surface
				|| (!surface_node->surface->mapped && !collect->unmapped_too)) {
			break;
		}
		struct wlr_box node_clip = surface_node->clip;
		node_clip.x += x;
		node_clip.y += y;
		collect->x = x;
		collect->y = y;
		collect->clip = wlr_box_empty(&surface_node->clip) ? NULL : &node_clip;
		// a window rounded by itself (set_shape): all its surfaces cut to it,
		// its window geometry. filling the screen (fullscreen): no corners
		double radius = eyecandy_shape(surface_node->surface);
		if (radius > 0) {
			struct wlr_box window = collect->clip ? node_clip : (struct wlr_box){x, y,
				surface_node->surface->current.width, surface_node->surface->current.height};
			collect->shape = apply(&collect->t, window);
			collect->shape_radius = (float)(radius * collect->t.ax);
			struct wlr_box covered;
			if (wlr_box_intersection(&covered, &collect->shape, &collect->area)
					&& wlr_box_equal(&covered, &collect->area)) {
				collect->shape_radius = 0;
			}
		}
		// subsurfaces below, the surface, subsurfaces above. unmapped ones skipped
		wlr_surface_for_each_surface(surface_node->surface, collect_surface, collect);
		collect->clip = NULL;
		collect->shape_radius = 0;
		break;
	case FLAME_NODE_BUFFER:;
		struct flame_buffer_node *buffer = wl_container_of(node, buffer, node);
		struct wlr_box buffer_box = apply(&collect->t, (struct wlr_box){x, y, buffer->width,
			buffer->height});
		if (!buffer->texture || wlr_box_empty(&buffer_box)) {
			break;
		}
		if (!elements_push_texture(collect->list, buffer, buffer->texture, buffer->key,
				buffer_box, collect->area)) {
			collect->failed = true;
			break;
		}
		if (collect->list->len > 0) {
			struct flame_element *pushed = &collect->list->data[collect->list->len - 1];
			if (pushed->id == buffer) {
				mark(pushed, collect);
				pushed->opaque_texture = buffer->opaque;
				if (!wlr_fbox_empty(&buffer->src)) {
					pushed->src = buffer->src;
				}
				pushed->buffer_node = buffer;
				pushed->wait_timeline = buffer->wait_timeline;
				pushed->wait_point = buffer->wait_point;
				pushed->buffer = buffer->scanout_buffer;
			}
		}
		break;
	}
}

bool elements_collect(struct flame_element_list *list, struct flame_scene *scene,
		struct wlr_box area) {
	struct collect collect = {
		.list = list,
		.area = area,
		.t = {1, 0, 1, 0, 1},
	};
	collect_node(&collect, &scene->root.node, 0, 0);
	if (collect.failed) {
		wlr_log(WLR_ERROR, "out of memory building a frame");
	}
	return !collect.failed;
}

bool elements_collect_node(struct flame_element_list *list, struct flame_node *node,
		struct wlr_box area) {
	struct collect collect = {
		.list = list,
		.area = area,
		.t = {1, 0, 1, 0, 1},
		.unmapped_too = true,
	};
	// its own effect left out too: the snapshot is how it looks plain
	bool had = false;
	struct flame_tree *tree = node->type == FLAME_NODE_TREE ? scene_tree_from_node(node) : NULL;
	if (tree) {
		had = tree->has_effect;
		tree->has_effect = false;
	}
	bool enabled = node->enabled;
	node->enabled = true;
	collect_node(&collect, node, 0, 0);
	node->enabled = enabled;
	if (tree) {
		tree->has_effect = had;
	}
	return !collect.failed;
}

bool elements_push_surface(struct flame_element_list *list, struct wlr_surface *surface,
		int x, int y, struct wlr_box area) {
	struct collect collect = {
		.list = list,
		.area = area,
		.x = x,
		.y = y,
		.t = {1, 0, 1, 0, 1},
	};
	wlr_surface_for_each_surface(surface, collect_surface, &collect);
	return !collect.failed;
}

bool elements_push_texture(struct flame_element_list *list, const void *id,
		struct wlr_texture *texture, const void *image, struct wlr_box box, struct wlr_box area) {
	struct wlr_box clip;
	if (wlr_box_empty(&box) || !wlr_box_intersection(&clip, &box, &area)) {
		return true;
	}
	struct flame_element *element = push(list);
	if (!element) {
		return false;
	}
	element->type = FLAME_ELEMENT_TEXTURE;
	element->id = id;
	element->box = box;
	element->clip = clip;
	element->texture = texture;
	element->image = image;
	element->src = (struct wlr_fbox){.width = texture->width, .height = texture->height};
	element->transform = WL_OUTPUT_TRANSFORM_NORMAL;
	return true;
}

void elements_occlude(struct flame_element_list *list) {
	pixman_region32_t above;
	pixman_region32_init(&above);

	for (size_t i = list->len; i-- > 0;) {
		struct flame_element *element = &list->data[i];
		struct wlr_box clip = element->clip;
		pixman_region32_union_rect(&element->visible, &element->visible, clip.x, clip.y,
			clip.width, clip.height);
		pixman_region32_subtract(&element->visible, &element->visible, &above);

		if (element->effected || element->clear) {
			// faded or scaled: see through, and its opaque region doesn't fit
			// the box anymore. clear: nothing there to cover anything
		} else if (element->type == FLAME_ELEMENT_RECT) {
			if (element->color[3] >= 1.0f) {
				pixman_region32_union_rect(&element->opaque, &element->opaque, clip.x,
					clip.y, clip.width, clip.height);
			}
		} else if (element->type == FLAME_ELEMENT_TEXTURE) {
			if (element->opaque_texture) {
				pixman_region32_union_rect(&element->opaque, &element->opaque, clip.x,
					clip.y, clip.width, clip.height);
			}
		} else if (element->type == FLAME_ELEMENT_SURFACE) {
			// whatever the client says is opaque, inside what can show
			pixman_region32_copy(&element->opaque, &element->surface->opaque_region);
			pixman_region32_translate(&element->opaque, element->box.x, element->box.y);
			pixman_region32_intersect_rect(&element->opaque, &element->opaque, clip.x,
				clip.y, clip.width, clip.height);
			if (element->shape_radius > 0) {
				// its window's corners show what's behind them
				struct wlr_box s = element->shape;
				int r = (int)ceilf(element->shape_radius);
				pixman_region32_t corners;
				pixman_region32_init_rect(&corners, s.x, s.y, r, r);
				pixman_region32_union_rect(&corners, &corners, s.x + s.width - r, s.y, r, r);
				pixman_region32_union_rect(&corners, &corners, s.x, s.y + s.height - r, r, r);
				pixman_region32_union_rect(&corners, &corners, s.x + s.width - r,
					s.y + s.height - r, r, r);
				pixman_region32_subtract(&element->opaque, &element->opaque, &corners);
				pixman_region32_fini(&corners);
			}
		}
		pixman_region32_union(&above, &above, &element->opaque);
	}
	pixman_region32_fini(&above);
}

int elements_glass_reach(const struct flame_element *element) {
	if (element->type != FLAME_ELEMENT_GLASS) {
		return 0;
	}
	// a blur spreads about twice its radius before it fades out
	return (int)ceil(2 * element->glass.blur + element->glass.refraction) + 1;
}

const struct flame_element *elements_find(const struct flame_element_list *list,
		const void *id, size_t hint, size_t *index) {
	if (hint < list->len && list->data[hint].id == id) {
		*index = hint;
		return &list->data[hint];
	}
	for (size_t i = 0; i < list->len; i++) {
		if (list->data[i].id == id) {
			*index = i;
			return &list->data[i];
		}
	}
	return NULL;
}

static uint64_t mix(uint64_t hash, uint64_t value) {
	return (hash ^ value) * 0x100000001b3ull;
}

static uint64_t float_bits(float value) {
	uint32_t bits;
	memcpy(&bits, &value, sizeof(bits));
	return bits;
}

uint64_t elements_look(const struct flame_element_list *list, struct wlr_box area,
		float scale) {
	uint64_t hash = 0xcbf29ce484222325ull;
	hash = mix(hash, (uint64_t)area.width << 32 | (uint32_t)area.height);
	hash = mix(hash, float_bits(scale));
	for (size_t i = 0; i < list->len; i++) {
		const struct flame_element *e = &list->data[i];
		hash = mix(hash, (uint64_t)e->type);
		hash = mix(hash, (uintptr_t)e->id);
		hash = mix(hash, (uint64_t)(uint32_t)e->box.x << 32 | (uint32_t)e->box.y);
		hash = mix(hash, (uint64_t)(uint32_t)e->box.width << 32 | (uint32_t)e->box.height);
		hash = mix(hash, (uint64_t)(uint32_t)e->clip.width << 32 | (uint32_t)e->clip.height);
		hash = mix(hash, (uintptr_t)e->buffer);
		hash = mix(hash, (uintptr_t)e->image);
		hash = mix(hash, (uintptr_t)e->texture);
		hash = mix(hash, e->surface ? e->surface->current.seq : 0);
		hash = mix(hash, float_bits(e->alpha));
		for (int c = 0; c < 4; c++) {
			hash = mix(hash, float_bits(e->color[c]));
			hash = mix(hash, float_bits(e->glass.tint[c]));
		}
		hash = mix(hash, float_bits((float)e->glass.radius));
		hash = mix(hash, float_bits((float)e->glass.blur));
		hash = mix(hash, float_bits(e->glass.saturation));
		hash = mix(hash, float_bits(e->glass.edge));
		hash = mix(hash, float_bits((float)e->glass.refraction));
	}
	return hash;
}
