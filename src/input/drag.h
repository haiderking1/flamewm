#ifndef FLAME_INPUT_DRAG_H
#define FLAME_INPUT_DRAG_H

#include <stdbool.h>
#include <stdint.h>

struct flame_server;
struct flame_view;

// mod + left drag a tiled window, drop it on another one to swap them.
// same as raven: the window rides the cursor on top of everything, its slot
// stays empty, and the swap lands as one transaction
struct flame_drag {
	struct flame_server *server;
	struct flame_view *view; // NULL when nothing is held
	uint32_t button;
	double start_x, start_y; // cursor when it started
	int origin_x, origin_y; // where the window sat when it started
	// mouse buttons whose press we ate, so only their releases get eaten too
	uint32_t swallowed;
};

void drag_init(struct flame_drag *drag, struct flame_server *server);
bool drag_active(struct flame_drag *drag);

// true when the drag owns this button event and the client must not see it
bool drag_button(struct flame_drag *drag, uint32_t button, bool pressed);
void drag_motion(struct flame_drag *drag);

// put it back where it was, nothing swaps
void drag_cancel(struct flame_drag *drag);
// if view is the one being dragged, let go of it
void drag_forget(struct flame_drag *drag, struct flame_view *view);

#endif
