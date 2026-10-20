#ifndef FLAME_SHELL_DECORATION_H
#define FLAME_SHELL_DECORATION_H

#include <stdbool.h>
#include <wayland-server-core.h>

struct wl_display;

// xdg-decoration: tell every client we draw the decorations (just borders),
// so no csd titlebars eating tile space
struct flame_decorations {
	struct wlr_xdg_decoration_manager_v1 *manager;
	struct wl_listener new_decoration;
};

bool decorations_init(struct flame_decorations *decorations, struct wl_display *display);
void decorations_finish(struct flame_decorations *decorations);

#endif
