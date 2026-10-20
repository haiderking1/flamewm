#ifndef FLAME_INPUT_DRAG_H
#define FLAME_INPUT_DRAG_H

#include <stdbool.h>
#include <stdint.h>
#include <wlr/util/box.h>

struct flame_server;
struct flame_view;
struct tree_node;

// mod + mouse on a floating window moves it (left) or resizes it from the
// nearest corner (right). on a tiled window, like hyprland and raven:
// - left drag picks it up, it rides the cursor on top of everything, its slot
//   stays empty, and dropping it on another window swaps the two
// - right drag resizes in place: the edges nearest to where you grabbed follow
//   the cursor by moving the tree splits they sit on
// moves land through a transaction, so both windows move in the same frame.
// resizes don't wait on anyone: the edges follow the cursor once per output
// frame and clients catch up as fast as they redraw, like hyprland

enum flame_drag_mode {
	FLAME_DRAG_MOVE,
	FLAME_DRAG_RESIZE,
};

// one axis of a resize: the split whose dividing line we're moving
struct flame_drag_axis {
	struct tree_node *split; // NULL when this axis can't move
	int start_size; // its first child's size when the drag started
};

struct flame_drag {
	struct flame_server *server;
	struct flame_view *view; // NULL when nothing is held
	enum flame_drag_mode mode;
	uint32_t button;
	double start_x, start_y; // cursor when it started
	int origin_x, origin_y; // where the window sat when it started (move)
	struct flame_drag_axis horizontal, vertical; // (resize)
	// a floating window: its box when it started, which corner follows
	struct wlr_box float_start;
	bool float_right, float_bottom;
	bool resize_pending; // the cursor moved since the last frame (resize)
	// mouse buttons whose press we ate, so only their releases get eaten too
	uint32_t swallowed;
};

void drag_init(struct flame_drag *drag, struct flame_server *server);
bool drag_active(struct flame_drag *drag);

// true when the drag owns this button event and the client must not see it
bool drag_button(struct flame_drag *drag, uint32_t button, bool pressed);
void drag_motion(struct flame_drag *drag);
// an output is about to draw, a resize catches up with the cursor here
void drag_frame(struct flame_drag *drag);

// put it back where it was, nothing swaps
void drag_cancel(struct flame_drag *drag);
// if view is the one being dragged, let go of it
void drag_forget(struct flame_drag *drag, struct flame_view *view);
// a workspace tree changed shape, a resize holding its splits has to stop
void drag_tree_changed(struct flame_drag *drag);

#endif
