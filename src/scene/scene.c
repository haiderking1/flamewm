#include "scene/scene.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_compositor.h>

#include "scene/surface.h"
#include "util/listener.h"

static void node_init(struct flame_node *node, enum flame_node_type type,
		struct flame_scene *scene, struct flame_tree *parent) {
	node->type = type;
	node->scene = scene;
	node->parent = parent;
	node->enabled = true;
	wl_signal_init(&node->events.destroy);
	if (parent) {
		wl_list_insert(parent->children.prev, &node->link);
	} else {
		wl_list_init(&node->link);
	}
}

void scene_changed(struct flame_scene *scene) {
	wl_signal_emit_mutable(&scene->events.changed, scene);
}

void scene_init(struct flame_scene *scene) {
	memset(scene, 0, sizeof(*scene));
	wl_signal_init(&scene->events.changed);
	wl_signal_init(&scene->events.damage);
	wl_signal_init(&scene->events.surface_destroy);
	node_init(&scene->root.node, FLAME_NODE_TREE, scene, NULL);
	wl_list_init(&scene->root.children);
}

void scene_finish(struct flame_scene *scene) {
	struct flame_node *child, *tmp;
	wl_list_for_each_safe(child, tmp, &scene->root.children, link) {
		scene_node_destroy(child);
	}
}

struct flame_tree *scene_tree_from_node(struct flame_node *node) {
	assert(node->type == FLAME_NODE_TREE);
	struct flame_tree *tree = wl_container_of(node, tree, node);
	return tree;
}

struct flame_tree *scene_tree_create(struct flame_tree *parent) {
	struct flame_tree *tree = calloc(1, sizeof(*tree));
	if (!tree) {
		return NULL;
	}
	node_init(&tree->node, FLAME_NODE_TREE, parent->node.scene, parent);
	wl_list_init(&tree->children);
	return tree;
}

void scene_tree_set_effect(struct flame_tree *tree, const struct flame_effect *effect) {
	bool had = tree->has_effect;
	tree->has_effect = effect != NULL;
	if (effect) {
		tree->effect = *effect;
	}
	if (had || effect) {
		scene_changed(tree->node.scene);
	}
}

struct flame_rect *scene_rect_create(struct flame_tree *parent, int width, int height,
		const float color[4]) {
	struct flame_rect *rect = calloc(1, sizeof(*rect));
	if (!rect) {
		return NULL;
	}
	node_init(&rect->node, FLAME_NODE_RECT, parent->node.scene, parent);
	rect->width = width;
	rect->height = height;
	memcpy(rect->color, color, sizeof(rect->color));
	scene_changed(rect->node.scene);
	return rect;
}

// the surface's record points back at the node drawing it, so its commits
// know whether they're on a visible workspace
static void unlink_state(struct flame_surface_node *node) {
	struct flame_surface_state *state =
		node->surface ? scene_surface_state(node->surface) : NULL;
	if (state && state->node == node) {
		state->node = NULL;
	}
}

static void handle_surface_destroy(struct wl_listener *listener, void *data) {
	struct flame_surface_node *node = wl_container_of(listener, node, surface_destroy);
	unlink_state(node);
	// whoever owns the node destroys it with the role object, until then it
	// just draws nothing
	listener_disconnect(&node->surface_destroy);
	node->surface = NULL;
	scene_changed(node->node.scene);
}

struct flame_surface_node *scene_surface_create(struct flame_tree *parent,
		struct wlr_surface *surface) {
	struct flame_surface_node *node = calloc(1, sizeof(*node));
	if (!node) {
		return NULL;
	}
	node_init(&node->node, FLAME_NODE_SURFACE, parent->node.scene, parent);
	node->surface = surface;
	listener_connect(&node->surface_destroy, &surface->events.destroy,
		handle_surface_destroy);
	struct flame_surface_state *state = scene_surface_state(surface);
	if (state && !state->node) {
		state->node = node;
	}
	scene_changed(node->node.scene);
	return node;
}

struct flame_buffer_node *scene_buffer_create(struct flame_tree *parent,
		const struct flame_buffer_node_impl *impl, void *data) {
	struct flame_buffer_node *node = calloc(1, sizeof(*node));
	if (!node) {
		return NULL;
	}
	node_init(&node->node, FLAME_NODE_BUFFER, parent->node.scene, parent);
	node->impl = impl;
	node->data = data;
	return node;
}

void scene_buffer_set(struct flame_buffer_node *node, struct wlr_texture *texture,
		const void *key, int width, int height, bool opaque,
		struct wlr_drm_syncobj_timeline *wait_timeline, uint64_t wait_point) {
	node->texture = texture;
	node->key = key;
	node->width = width;
	node->height = height;
	node->opaque = opaque;
	node->wait_timeline = wait_timeline;
	node->wait_point = wait_point;
	scene_changed(node->node.scene);
}

void scene_buffer_set_source(struct flame_buffer_node *node, const struct wlr_fbox *src) {
	node->src = src ? *src : (struct wlr_fbox){0};
	scene_changed(node->node.scene);
}

void scene_node_destroy(struct flame_node *node) {
	if (!node) {
		return;
	}
	// owners hanging on to it let go here, before anything is freed
	wl_signal_emit_mutable(&node->events.destroy, node);

	switch (node->type) {
	case FLAME_NODE_TREE:;
		struct flame_tree *tree = scene_tree_from_node(node);
		struct flame_node *child, *tmp;
		wl_list_for_each_safe(child, tmp, &tree->children, link) {
			scene_node_destroy(child);
		}
		break;
	case FLAME_NODE_SURFACE:;
		struct flame_surface_node *surface = wl_container_of(node, surface, node);
		unlink_state(surface);
		listener_disconnect(&surface->surface_destroy);
		break;
	case FLAME_NODE_RECT:
	case FLAME_NODE_BUFFER:
		break;
	}

	struct flame_scene *scene = node->scene;
	wl_list_remove(&node->link);
	assert(wl_list_empty(&node->events.destroy.listener_list));
	if (node == &scene->root.node) {
		return; // embedded in the scene
	}
	free(node);
	scene_changed(scene);
}

void scene_node_set_enabled(struct flame_node *node, bool enabled) {
	if (node->enabled != enabled) {
		node->enabled = enabled;
		scene_changed(node->scene);
	}
}

void scene_node_set_position(struct flame_node *node, int x, int y) {
	if (node->x != x || node->y != y) {
		node->x = x;
		node->y = y;
		scene_changed(node->scene);
	}
}

// only the assert below uses it, release builds drop that
__attribute__((unused))
static bool is_ancestor(struct flame_node *node, struct flame_tree *tree) {
	for (struct flame_tree *t = tree; t; t = t->node.parent) {
		if (&t->node == node) {
			return true;
		}
	}
	return false;
}

void scene_node_reparent(struct flame_node *node, struct flame_tree *parent) {
	assert(node->parent && !is_ancestor(node, parent));
	if (node->parent == parent) {
		return;
	}
	wl_list_remove(&node->link);
	wl_list_insert(parent->children.prev, &node->link);
	node->parent = parent;
	scene_changed(node->scene);
}

void scene_node_raise_to_top(struct flame_node *node) {
	struct wl_list *children = &node->parent->children;
	if (children->prev == &node->link) {
		return;
	}
	wl_list_remove(&node->link);
	wl_list_insert(children->prev, &node->link);
	scene_changed(node->scene);
}

void scene_node_lower_to_bottom(struct flame_node *node) {
	struct wl_list *children = &node->parent->children;
	if (children->next == &node->link) {
		return;
	}
	wl_list_remove(&node->link);
	wl_list_insert(children, &node->link);
	scene_changed(node->scene);
}

bool scene_node_coords(struct flame_node *node, int *lx, int *ly) {
	int x = 0, y = 0;
	bool enabled = true;
	for (struct flame_node *n = node; n; n = n->parent ? &n->parent->node : NULL) {
		x += n->x;
		y += n->y;
		enabled &= n->enabled;
	}
	*lx = x;
	*ly = y;
	return enabled;
}

void scene_rect_set_size(struct flame_rect *rect, int width, int height) {
	if (rect->width != width || rect->height != height) {
		rect->width = width;
		rect->height = height;
		scene_changed(rect->node.scene);
	}
}

void scene_rect_set_color(struct flame_rect *rect, const float color[4]) {
	if (memcmp(rect->color, color, sizeof(rect->color))) {
		memcpy(rect->color, color, sizeof(rect->color));
		scene_changed(rect->node.scene);
	}
}

void scene_surface_set_clip(struct flame_surface_node *node, const struct wlr_box *clip) {
	struct wlr_box box = clip ? *clip : (struct wlr_box){0};
	if (!wlr_box_equal(&box, &node->clip)) {
		node->clip = box;
		scene_changed(node->node.scene);
	}
}

static struct flame_node *node_at(struct flame_node *node, int x, int y, double lx,
		double ly, struct wlr_surface **surface, double *sx, double *sy) {
	if (!node->enabled || node->passthrough) {
		return NULL;
	}
	x += node->x;
	y += node->y;

	switch (node->type) {
	case FLAME_NODE_TREE:;
		struct flame_tree *tree = scene_tree_from_node(node);
		struct flame_node *child;
		wl_list_for_each_reverse(child, &tree->children, link) {
			struct flame_node *hit = node_at(child, x, y, lx, ly, surface, sx, sy);
			if (hit) {
				return hit;
			}
		}
		return NULL;
	case FLAME_NODE_RECT:;
		struct flame_rect *rect = wl_container_of(node, rect, node);
		struct wlr_box box = {x, y, rect->width, rect->height};
		if (wlr_box_contains_point(&box, lx, ly)) {
			*surface = NULL;
			return node;
		}
		return NULL;
	case FLAME_NODE_SURFACE:;
		struct flame_surface_node *surface_node = wl_container_of(node, surface_node, node);
		if (!surface_node->surface || !surface_node->surface->mapped) {
			return NULL;
		}
		double local_x = lx - x, local_y = ly - y;
		if (!wlr_box_empty(&surface_node->clip)
				&& !wlr_box_contains_point(&surface_node->clip, local_x, local_y)) {
			return NULL;
		}
		// subsurfaces and input regions included
		struct wlr_surface *hit = wlr_surface_surface_at(surface_node->surface,
			local_x, local_y, sx, sy);
		if (hit) {
			*surface = hit;
			return node;
		}
		return NULL;
	case FLAME_NODE_BUFFER:;
		struct flame_buffer_node *buffer = wl_container_of(node, buffer, node);
		struct wlr_box area = {x, y, buffer->width, buffer->height};
		if (!buffer->texture || !wlr_box_contains_point(&area, lx, ly)) {
			return NULL;
		}
		if (buffer->impl && buffer->impl->accepts_input
				&& !buffer->impl->accepts_input(buffer, lx - x, ly - y)) {
			return NULL;
		}
		*surface = NULL;
		*sx = lx - x;
		*sy = ly - y;
		return node;
	}
	return NULL;
}

struct flame_node *scene_node_at(struct flame_scene *scene, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
	*surface = NULL;
	return node_at(&scene->root.node, 0, 0, lx, ly, surface, sx, sy);
}
