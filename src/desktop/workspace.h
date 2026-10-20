#ifndef FLAME_DESKTOP_WORKSPACE_H
#define FLAME_DESKTOP_WORKSPACE_H

#include <stdbool.h>
#include <wayland-server-core.h>

#define FLAME_WORKSPACE_COUNT 10

struct flame_view;
struct tree_node;

struct flame_workspace {
	int index;
	struct flame_tree *tree; // tiles, disabled while hidden
	struct flame_tree *fullscreen_tree; // above the bars, hidden with the rest
	struct flame_rect *backdrop; // black behind a fullscreen window
	struct tree_node *layout; // bsp tree, NULL when empty
	struct wl_list views; // flame_view.link, in leaf order: the tiles
	// floating windows: not in the tree, above the tiles, each where it is.
	// flame_view.link, bottom to top as last raised
	struct wl_list floats;
	struct flame_tree *float_tree;
	struct flame_view *last_focused;
	// one fullscreen window per workspace. it keeps its leaf, so leaving
	// fullscreen drops it right back into its tile
	struct flame_view *fullscreen;
};

void workspace_init(struct flame_workspace *workspace, int index,
	struct flame_tree *tiled, struct flame_tree *floating, struct flame_tree *fullscreen);
void workspace_set_visible(struct flame_workspace *workspace, bool visible);

// the leaf a new window splits: the focused one, else the last
struct tree_node *workspace_insert_target(struct flame_workspace *workspace);

bool workspace_add_view(struct flame_workspace *workspace, struct flame_view *view);
// a floating window, on top of the others floating here
void workspace_add_float(struct flame_workspace *workspace, struct flame_view *view);
// tiles and floats both
int workspace_view_count(struct flame_workspace *workspace);
void workspace_remove_view(struct flame_workspace *workspace, struct flame_view *view);

// both must live here. trades their tiles and their spot in the list
void workspace_swap_views(struct flame_workspace *workspace, struct flame_view *a,
	struct flame_view *b);

struct flame_view *workspace_first_view(struct flame_workspace *workspace);

#endif
