#ifndef FLAME_LAYERS_LAYERS_H
#define FLAME_LAYERS_LAYERS_H

#include <stdbool.h>
#include <wayland-server-core.h>

#include "wlr-layer-shell-unstable-v1-protocol.h"

struct flame_output;
struct flame_server;
struct wlr_surface;

// layer-shell: bars, launchers, wallpapers, notifications. they live on an
// output, never tile, and can reserve screen edges so tiles stay clear

struct flame_layer_surface {
	struct wl_list link; // flame_layers.surfaces
	struct flame_layers *layers;
	struct wlr_layer_surface_v1 *wlr; // wlr->data points back here
	struct flame_output *output; // NULL once its output is gone
	struct wlr_scene_layer_surface_v1 *scene;
	// its popups, kept above the tiles even when the surface sits below them
	struct wlr_scene_tree *popups;
	enum zwlr_layer_shell_v1_layer layer; // scene tree it's in right now

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener new_popup;
	struct wl_listener destroy;
};

struct flame_layers {
	struct flame_server *server;
	struct wlr_layer_shell_v1 *shell;
	struct wl_list surfaces; // flame_layer_surface.link
	struct flame_layer_surface *focused; // has the keyboard, NULL when a window does

	struct wl_listener new_surface;
};

bool layers_init(struct flame_layers *layers, struct flame_server *server);
void layers_finish(struct flame_layers *layers);

// output going away takes its layer surfaces with it
void layers_output_gone(struct flame_layers *layers, struct flame_output *output);

// arrange.c: place everything on output and recompute its usable area.
// true when the usable area changed and the tiles need to move
bool layers_arrange(struct flame_layers *layers, struct flame_output *output);

// focus.c
void layers_update_focus(struct flame_layers *layers);
// an exclusive layer (open launcher) owns the keyboard, windows can't take it
bool layers_keyboard_locked(struct flame_layers *layers);
// a window got focused, an on demand layer lets go of the keyboard
void layers_drop_keyboard(struct flame_layers *layers);
void layers_click(struct flame_layers *layers, struct wlr_surface *surface);

struct flame_layer_surface *layer_surface_from_wlr(struct wlr_surface *surface);

#endif
