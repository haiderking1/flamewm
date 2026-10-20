#ifndef FLAME_LAYOUT_TREE_H
#define FLAME_LAYOUT_TREE_H

#include <stdbool.h>
#include <wlr/util/box.h>

struct flame_view;

// bspwm style binary space partition. pure data + math, knows nothing
// about the scene or protocols

enum tree_split {
	TREE_SPLIT_VERTICAL, // children side by side
	TREE_SPLIT_HORIZONTAL, // children stacked
};

struct tree_node {
	struct tree_node *parent;
	struct tree_node *first, *second; // both NULL on leaves
	struct flame_view *view; // leaves only

	enum tree_split split;
	float ratio; // first child's share

	struct wlr_box box; // from the last tree_layout(), gaps already applied
};

// splits target in two, target keeps the first half and the new leaf gets
// the second. NULL target splits the last leaf. NULL on alloc failure
struct tree_node *tree_insert(struct tree_node **root, struct tree_node *target,
	struct flame_view *view);
// the sibling takes over the space, leaf is freed
void tree_remove(struct tree_node **root, struct tree_node *leaf);

void tree_layout(struct tree_node *root, struct wlr_box area, int gap);
// the box a leaf inserted at target would get, without inserting it
struct wlr_box tree_prospective(struct tree_node *root, struct tree_node *target,
	struct wlr_box area, int gap);

struct tree_node *tree_last_leaf(struct tree_node *root);

// two leaves trade windows, the tree's shape stays the same
void tree_swap(struct tree_node *a, struct tree_node *b);

#endif
