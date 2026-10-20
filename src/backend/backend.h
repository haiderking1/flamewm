#ifndef FLAME_BACKEND_BACKEND_H
#define FLAME_BACKEND_BACKEND_H

#include <stdbool.h>
#include <wayland-server-core.h>

// hardware side: drm/libinput on a tty, or a window when nested
struct flame_backend {
	struct wlr_backend *wlr;
	struct wlr_session *session; // NULL when nested
};

bool backend_init(struct flame_backend *backend, struct wl_event_loop *loop);
bool backend_start(struct flame_backend *backend);
void backend_finish(struct flame_backend *backend);

bool backend_change_vt(struct flame_backend *backend, unsigned vt);

#endif
