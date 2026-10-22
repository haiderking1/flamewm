#ifndef FLAME_EYECANDY_EYECANDY_H
#define FLAME_EYECANDY_EYECANDY_H

#include <stdbool.h>
#include <stddef.h>
#include <wlr/util/box.h>

#include "scene/scene.h"

struct flame_server;
struct wlr_surface;

// glass behind apps' surfaces, drawn by flame's renderer like its own:
// flame-eyecandy-v1 (rounded panes, each with all of the glass's look) and
// ext-background-effect-v1 (a blur region, as flame's glass in a plain
// look). the frame draws a surface's panes right under it
struct flame_eyecandy {
	struct flame_server *server;
	struct wl_global *eyecandy, *background;
};

// one pane, surface local
struct eyecandy_pane {
	struct wlr_box box;
	struct flame_glass_style style;
};

bool eyecandy_init(struct flame_eyecandy *eyecandy, struct flame_server *server);
void eyecandy_finish(struct flame_eyecandy *eyecandy);

// surface is a whole window with a rounded edge of its own (set_shape):
// flame draws no border around it
bool eyecandy_shaped(struct wlr_surface *surface);
// surface's panes as last committed, bottom to top, NULL with none. good
// till the surface's next commit
const struct eyecandy_pane *eyecandy_panes(struct wlr_surface *surface, size_t *len);

#endif
