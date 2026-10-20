#include "render/elements.h"

#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/util/log.h>

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
	pixman_region32_init(&element->visible);
	pixman_region32_init(&element->opaque);
	return element;
}

struct collect {
	struct flame_element_list *list;
	struct wlr_box area;
	// the surface node being flattened
	int x, y;
	const struct wlr_box *clip; // layout coords, NULL for none
	bool failed;
};

// box cut down to what can show on this output, false when nothing can
static bool clip_box(const struct collect *collect, struct wlr_box box,
		const struct wlr_box *clip, struct wlr_box *out) {
	if (!wlr_box_intersection(out, &box, &collect->area)) {
		return false;
	}
	return !clip || wlr_box_intersection(out, out, clip);
}

static void collect_surface(struct wlr_surface *surface, int sx, int sy, void *data) {
	struct collect *collect = data;
	struct wlr_texture *texture = wlr_surface_get_texture(surface);
	if (!texture || collect->failed) {
		return;
	}
	struct wlr_box box = {
		.x = collect->x + sx,
		.y = collect->y + sy,
		.width = surface->current.width,
		.height = surface->current.height,
	};
	struct wlr_box clip;
	if (wlr_box_empty(&box) || !clip_box(collect, box, collect->clip, &clip)) {
		return;
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
	element->surface = surface;
	element->texture = texture;
	wlr_surface_get_buffer_source_box(surface, &element->src);
	element->transform = surface->current.transform;
	element->buffer = surface->buffer ? &surface->buffer->base : NULL;
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
		struct flame_node *child;
		wl_list_for_each(child, &tree->children, link) {
			collect_node(collect, child, x, y);
		}
		break;
	case FLAME_NODE_RECT:;
		struct flame_rect *rect = wl_container_of(node, rect, node);
		struct wlr_box box = {x, y, rect->width, rect->height};
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
		break;
	case FLAME_NODE_SURFACE:;
		struct flame_surface_node *surface_node = wl_container_of(node, surface_node, node);
		if (!surface_node->surface || !surface_node->surface->mapped) {
			break;
		}
		struct wlr_box node_clip = surface_node->clip;
		node_clip.x += x;
		node_clip.y += y;
		collect->x = x;
		collect->y = y;
		collect->clip = wlr_box_empty(&surface_node->clip) ? NULL : &node_clip;
		// subsurfaces below, the surface, subsurfaces above. unmapped ones skipped
		wlr_surface_for_each_surface(surface_node->surface, collect_surface, collect);
		collect->clip = NULL;
		break;
	}
}

bool elements_collect(struct flame_element_list *list, struct flame_scene *scene,
		struct wlr_box area) {
	struct collect collect = {
		.list = list,
		.area = area,
	};
	collect_node(&collect, &scene->root.node, 0, 0);
	if (collect.failed) {
		wlr_log(WLR_ERROR, "out of memory building a frame");
	}
	return !collect.failed;
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

		if (element->type == FLAME_ELEMENT_RECT) {
			if (element->color[3] >= 1.0f) {
				pixman_region32_union_rect(&element->opaque, &element->opaque, clip.x,
					clip.y, clip.width, clip.height);
			}
		} else {
			// whatever the client says is opaque, inside what can show
			pixman_region32_copy(&element->opaque, &element->surface->opaque_region);
			pixman_region32_translate(&element->opaque, element->box.x, element->box.y);
			pixman_region32_intersect_rect(&element->opaque, &element->opaque, clip.x,
				clip.y, clip.width, clip.height);
		}
		pixman_region32_union(&above, &above, &element->opaque);
	}
	pixman_region32_fini(&above);
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
