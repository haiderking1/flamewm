#ifndef FLAME_DESKTOP_FLOATING_H
#define FLAME_DESKTOP_FLOATING_H

#include <stdbool.h>
#include <wlr/util/box.h>

struct flame_desktop;
struct flame_view;

// windows that float above the tiles: dialogs and fixed size windows on
// their own, anything else when the user toggles it. each keeps its own
// box, moved and resized with the mouse

// where a window starts floating: the size it drew (or wants), centered on
// the output, border added. a window that floated before goes back there
struct wlr_box floating_initial_box(struct flame_desktop *desktop, struct flame_view *view);

// tile it, or float it
void floating_toggle(struct flame_desktop *desktop, struct flame_view *view);

// a floating window's new box, border included: from a mouse drag (live,
// nothing waits) or the client asking for a size of its own
void floating_set_box(struct flame_desktop *desktop, struct flame_view *view,
	struct wlr_box box, bool live);

// the client drew a size of its own: a floating window takes it
void floating_content_resized(struct flame_desktop *desktop, struct flame_view *view,
	int width, int height);

#endif
