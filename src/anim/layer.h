#ifndef FLAME_ANIM_LAYER_H
#define FLAME_ANIM_LAYER_H

struct flame_layer_surface;
struct flame_layer_anim;

// bars, notifications and launchers animating in and out: sliding from the
// edge they're anchored to (the nearest one when they float in the middle),
// zooming or fading, like windows do

// it mapped and has its spot
void layer_anim_mapped(struct flame_layer_surface *surface);
// it's about to go: its last picture, while the buffer's still there
void layer_anim_snapshot(struct flame_layer_surface *surface);
// it's gone: the close plays from the snapshot
void layer_anim_unmapped(struct flame_layer_surface *surface);
void layer_anim_free(struct flame_layer_surface *surface);

#endif
