#ifndef FLAME_DESKTOP_FOCUS_H
#define FLAME_DESKTOP_FOCUS_H

struct flame_desktop;
struct flame_view;
struct flame_workspace;

// NULL clears focus
void focus_view(struct flame_desktop *desktop, struct flame_view *view);
// keyboard back to the focused window, unless a layer surface holds it
void focus_restore_keyboard(struct flame_desktop *desktop);
// +1 next, -1 previous, wraps around
void focus_cycle(struct flame_desktop *desktop, int direction);
// who should get focus when a workspace shows up or loses its focused view
struct flame_view *focus_fallback(struct flame_workspace *workspace);

#endif
