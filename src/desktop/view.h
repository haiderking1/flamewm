#ifndef FLAME_DESKTOP_VIEW_H
#define FLAME_DESKTOP_VIEW_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wlr/util/box.h>

#include "config/config.h"
#include "transaction/transaction.h"

struct flame_desktop;
struct flame_view;
struct flame_workspace;
struct tree_node;
struct flame_node;

// what a shell (xdg now, xwayland later) has to provide for its windows
struct flame_view_impl {
	// ask for a new content size and fullscreen state. returns the serial to
	// wait on, or 0 when there's nothing to wait for (nothing changed, or not
	// configurable yet)
	uint32_t (*configure)(struct flame_view *view, int width, int height, bool fullscreen);
	// called from client_commit: does the commit being made ack serial
	bool (*acked)(struct flame_view *view, uint32_t serial);
	// we now show the window at this content size (clipping and such)
	void (*set_display_size)(struct flame_view *view, int width, int height);
	// size of what the client actually drew, its window geometry
	void (*content_size)(struct flame_view *view, int *width, int *height);
	// where the window sits inside its root surface (csd shadows and such
	// around it), from the commit being made or the one on screen. empty
	// when the client never said, then it's the whole surface
	struct wlr_box (*window_geometry)(struct flame_view *view, bool pending);
	void (*set_activated)(struct flame_view *view, bool activated);
	// the user is dragging its size around, sizes will change every frame
	void (*set_resizing)(struct flame_view *view, bool resizing);
	void (*close)(struct flame_view *view);
	struct wlr_surface *(*surface)(struct flame_view *view);
	// what bars show. NULL when the client never said
	const char *(*app_id)(struct flame_view *view);
	const char *(*title)(struct flame_view *view);
	// optional: its content is on screen here now, layout coords
	void (*placed)(struct flame_view *view, struct wlr_box content);
	// optional: who types when it has focus, when that's not its surface
	struct wlr_surface *(*keyboard_surface)(struct flame_view *view);
	// optional: a button went down on surface, somewhere inside the view
	void (*pressed)(struct flame_view *view, struct wlr_surface *surface);
	// optional: the size it draws at on its own, whatever flame shows of it.
	// where a floating window starts
	void (*natural_size)(struct flame_view *view, int *width, int *height);
	// optional: the smallest content it can draw, 0 where it doesn't say
	void (*min_size)(struct flame_view *view, int *width, int *height);
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

	struct flame_tree *tree; // node.data points back here
	struct flame_rect *border[FLAME_BORDER_COUNT];
	struct flame_tree *content; // the shell puts its surface tree in here

	// what's on screen, border included. targets live in txn until applied
	struct wlr_box geometry;
	bool shown;
	// held by a mouse drag, the drag owns where its tree sits
	bool grabbed;

	// floating: no tile, its own box above the tiles. float_box is where,
	// border included, layout coords. float_wanted: the shell says it should
	// float when it maps (dialogs, fixed size windows). has_float_box: it
	// floated before, toggling back puts it where it was
	bool floating, float_wanted, has_float_box;
	struct wlr_box float_box;

	// the client or the user wants it fullscreen
	bool fullscreen_wanted;
	// on screen as fullscreen right now, flips only when a transaction lands
	bool fullscreen_shown;
	// when it last mapped (now_ns): just opened, it may still take the
	// screen while the user's elsewhere
	int64_t mapped_at;

	// told it's in an interactive resize. terminals hold back the size from
	// their shell meanwhile, so it doesn't redraw its prompt every frame
	bool resizing;

	// leaving without a close animation: it's turning into something else
	bool unmap_quietly;
	// the next placement doesn't animate the move: the mouse put it there
	bool placed_by_hand;
	// shrinking back from fullscreen: still over the bars till it's in place
	bool leaving_fullscreen;
	// what init.lua's rules said when it opened, and its animations
	struct flame_window_rule rule;
	struct flame_view_anim *anim;

	struct flame_view_txn txn;
};

void view_init(struct flame_view *view, const struct flame_view_impl *impl,
	struct flame_desktop *desktop);
void view_finish(struct flame_view *view);

// content size for a box. tiles lose their border, fullscreen gets it all
void view_content_size(struct flame_view *view, struct wlr_box box, bool fullscreen,
	int *width, int *height);
uint32_t view_configure(struct flame_view *view, struct wlr_box box, bool fullscreen);
// the border flame draws around it: none fullscreen, none for a window with
// a rounded edge of its own, else the config's
int view_border_width(struct flame_view *view, bool fullscreen);
// put it on screen in this box, only the transaction calls this
void view_place(struct flame_view *view, struct wlr_box box, bool fullscreen);
void view_hide(struct flame_view *view);
// into the tree it belongs in: its workspace's tiles or fullscreen tree
void view_reparent(struct flame_view *view);
// done shrinking back from fullscreen: under the bars again
void view_left_fullscreen(struct flame_view *view);
// the client drew a new size, recenter it if it's smaller than fullscreen
void view_content_changed(struct flame_view *view);

void view_set_focused(struct flame_view *view, bool focused);
// its border in the config's colors, after the config changed
void view_refresh_border(struct flame_view *view, bool focused);
void view_set_resizing(struct flame_view *view, bool resizing);
void view_close(struct flame_view *view);
struct wlr_surface *view_surface(struct flame_view *view);
// where the keyboard goes when it's focused
struct wlr_surface *view_keyboard_surface(struct flame_view *view);
// a click landed on surface inside it, before it gets focused
void view_pressed(struct flame_view *view, struct wlr_surface *surface);
const char *view_app_id(struct flame_view *view);
const char *view_title(struct flame_view *view);

// walks up the scene graph from any node to the view owning it
struct flame_view *view_from_node(struct flame_node *node);

#endif
