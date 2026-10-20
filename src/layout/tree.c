#include "layout/tree.h"

#include <stdlib.h>

#include "util/macros.h"

static bool is_leaf(const struct tree_node *node) {
	return !node->first;
}

// longest side gets cut, like bspwm's default scheme
static enum tree_split split_for(struct wlr_box box) {
	return box.width >= box.height ? TREE_SPLIT_VERTICAL : TREE_SPLIT_HORIZONTAL;
}

static void split_box(struct wlr_box box, enum tree_split split, float ratio, int gap,
		struct wlr_box *first, struct wlr_box *second) {
	*first = box;
	*second = box;
	if (split == TREE_SPLIT_VERTICAL) {
		first->width = (int)((box.width - gap) * ratio);
		second->x = box.x + first->width + gap;
		second->width = box.width - first->width - gap;
	} else {
		first->height = (int)((box.height - gap) * ratio);
		second->y = box.y + first->height + gap;
		second->height = box.height - first->height - gap;
	}
	first->width = MAX(first->width, 1);
	first->height = MAX(first->height, 1);
	second->width = MAX(second->width, 1);
	second->height = MAX(second->height, 1);
}

static struct wlr_box shrink(struct wlr_box area, int gap) {
	return (struct wlr_box){
		.x = area.x + gap,
		.y = area.y + gap,
		.width = MAX(area.width - 2 * gap, 1),
		.height = MAX(area.height - 2 * gap, 1),
	};
}

static void replace_child(struct tree_node **root, struct tree_node *old,
		struct tree_node *new) {
	struct tree_node *parent = old->parent;
	new->parent = parent;
	if (!parent) {
		*root = new;
	} else if (parent->first == old) {
		parent->first = new;
	} else {
		parent->second = new;
	}
}

struct tree_node *tree_last_leaf(struct tree_node *root) {
	if (!root) {
		return NULL;
	}
	while (!is_leaf(root)) {
		root = root->second;
	}
	return root;
}

void tree_swap(struct tree_node *a, struct tree_node *b) {
	struct flame_view *view = a->view;
	a->view = b->view;
	b->view = view;
}

struct tree_node *tree_edge_split(struct tree_node *leaf, enum tree_split split, bool far) {
	struct tree_node *child = leaf;
	for (struct tree_node *parent = leaf->parent; parent; parent = parent->parent) {
		if (parent->split == split
				&& (far ? parent->first == child : parent->second == child)) {
			return parent;
		}
		child = parent;
	}
	return NULL;
}

int tree_first_size(const struct tree_node *node) {
	return node->split == TREE_SPLIT_VERTICAL ? node->first->box.width : node->first->box.height;
}

void tree_set_first_size(struct tree_node *node, int size, int gap, int min) {
	int span = (node->split == TREE_SPLIT_VERTICAL ? node->box.width : node->box.height) - gap;
	if (span <= 2 * min) {
		return;
	}
	size = CLAMP(size, min, span - min);
	node->ratio = (float)size / (float)span;
}

struct tree_node *tree_insert(struct tree_node **root, struct tree_node *target,
		struct flame_view *view) {
	struct tree_node *leaf = calloc(1, sizeof(*leaf));
	if (!leaf) {
		return NULL;
	}
	leaf->view = view;

	if (!*root) {
		*root = leaf;
		return leaf;
	}
	if (!target) {
		target = tree_last_leaf(*root);
	}

	struct tree_node *parent = calloc(1, sizeof(*parent));
	if (!parent) {
		free(leaf);
		return NULL;
	}
	parent->split = split_for(target->box);
	parent->ratio = 0.5f;
	parent->box = target->box;

	replace_child(root, target, parent);
	parent->first = target;
	parent->second = leaf;
	target->parent = parent;
	leaf->parent = parent;
	return leaf;
}

void tree_remove(struct tree_node **root, struct tree_node *leaf) {
	struct tree_node *parent = leaf->parent;
	if (!parent) {
		*root = NULL;
		free(leaf);
		return;
	}

	struct tree_node *sibling = parent->first == leaf ? parent->second : parent->first;
	replace_child(root, parent, sibling);
	free(parent);
	free(leaf);
}

static void layout_node(struct tree_node *node, struct wlr_box box, int gap) {
	node->box = box;
	if (is_leaf(node)) {
		return;
	}
	struct wlr_box first, second;
	split_box(box, node->split, node->ratio, gap, &first, &second);
	layout_node(node->first, first, gap);
	layout_node(node->second, second, gap);
}

void tree_layout(struct tree_node *root, struct wlr_box area, int gap) {
	if (root) {
		layout_node(root, shrink(area, gap), gap);
	}
}

struct wlr_box tree_prospective(struct tree_node *root, struct tree_node *target,
		struct wlr_box area, int gap) {
	if (!root) {
		return shrink(area, gap);
	}
	if (!target) {
		target = tree_last_leaf(root);
	}
	struct wlr_box first, second;
	split_box(target->box, split_for(target->box), 0.5f, gap, &first, &second);
	return second;
}
