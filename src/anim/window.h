#ifndef FLAME_ANIM_WINDOW_H
#define FLAME_ANIM_WINDOW_H

#include <stdbool.h>
#include <wlr/util/box.h>

struct flame_view;
struct flame_view_anim;

// windows animating: opening (slide, zoom or fade in), moving to a new box
// when the layout changes, closing (a snapshot of the last picture slides,
// shrinks or fades away), and fading with focus. drawn through an effect on
// the window's tree, where it really is never changes: input and layout
// don't wait for any of it

// view_place put it at box. was: where it was before, first: it just showed
void window_anim_placed(struct flame_view *view, struct wlr_box was, bool first);
// it's about to go: snapshot it now, while its buffers are still there.
// the close animation plays from that once it's gone
void window_anim_snapshot(struct flame_view *view);
// the same, only when there's no snapshot from before (taken when the
// client was about to let go of its buffer)
void window_anim_snapshot_if_missing(struct flame_view *view);
// it's gone from the screen (unmapped): the close animation plays
void window_anim_closed(struct flame_view *view);
// focus moved to it or away
void window_anim_focus(struct flame_view *view, bool focused);
// a mouse drag or resize has it: whatever's playing jumps to the end
void window_anim_stop(struct flame_view *view);
// a snapshot taken for a close that isn't coming
void window_anim_discard(struct flame_view *view);
void window_anim_free(struct flame_view *view);

#endif
