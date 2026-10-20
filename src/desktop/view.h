#ifndef FLAME_DESKTOP_VIEW_H
#define FLAME_DESKTOP_VIEW_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wlr/util/box.h>

#include "transaction/transaction.h"

struct flame_desktop;
struct flame_view;
struct flame_workspace;
struct tree_node;
struct wlr_scene_node;

// what a shell (xdg now, xwayland later) has to provide for its windows
struct flame_view_impl {
	// ask for a new content size. returns the serial to wait on, or 0 when
	// there's nothing to wait for (same size, or not configurable yet)
	uint32_t (*configure)(struct flame_view *view, int width, int height);
	// called from client_commit: does the commit being made ack serial
	bool (*acked)(struct flame_view *view, uint32_t serial);
	// we now show the window at this content size (clipping and such)
	void (*set_display_size)(struct flame_view *view, int width, int height);
	void (*set_activated)(struct flame_view *view, bool activated);
	void (*close)(struct flame_view *view);
	struct wlr_surface *(*surface)(struct flame_view *view);
};

enum flame_border_side {
	FLAME_BORDER_TOP,
	FLAME_BORDER_BOTTOM,
	FLAME_BORDER_LEFT,
	FLAME_BORDER_RIGHT,
	FLAME_BORDER_COUNT,
};

// a window as the desktop sees it, no idea which protocol is behind it
struct flame_view {
	const struct flame_view_impl *impl;
	struct flame_desktop *desktop;
	struct flame_workspace *workspace; // NULL while unmapped
	struct wl_list link; // flame_workspace.views, same order as the tree's leaves
	struct tree_node *node; // our leaf in the workspace tree

	struct wlr_scene_tree *tree; // node.data points back here
	struct wlr_scene_rect *border[FLAME_BORDER_COUNT];
	struct wlr_scene_tree *content; // the shell puts its surface tree in here

	// what's on screen, border included. targets live in txn until applied
	struct wlr_box geometry;
	bool shown;

	struct flame_view_txn txn;
};

void view_init(struct flame_view *view, const struct flame_view_impl *impl,
	struct flame_desktop *desktop);
void view_finish(struct flame_view *view);

// content size for a box that includes the border
void view_content_size(struct flame_view *view, struct wlr_box box, int *width, int *height);
uint32_t view_configure(struct flame_view *view, struct wlr_box box);
// put it on screen in this box, only the transaction calls this
void view_place(struct flame_view *view, struct wlr_box box);
void view_hide(struct flame_view *view);

void view_set_focused(struct flame_view *view, bool focused);
void view_close(struct flame_view *view);
struct wlr_surface *view_surface(struct flame_view *view);

// walks up the scene graph from any node to the view owning it
struct flame_view *view_from_node(struct wlr_scene_node *node);

#endif
