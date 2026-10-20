#ifndef FLAME_RENDER_ACQUIRE_H
#define FLAME_RENDER_ACQUIRE_H

#include <wayland-server-core.h>

struct wlr_compositor;

// client buffers only get used once the gpu finished writing them. a commit
// whose dmabuf is still being rendered waits, the window keeps its last frame
// on screen meanwhile, instead of us sampling it half drawn or our whole frame
// stalling on one slow client. the same thing mutter and kwin do, and raven.
// clients using explicit sync tell us when they're done themselves, they're
// left alone (wlr_linux_drm_syncobj_v1)
struct flame_acquire {
	struct wl_event_loop *loop;
	struct wl_listener new_surface;
};

void acquire_init(struct flame_acquire *acquire, struct wl_event_loop *loop,
	struct wlr_compositor *compositor);
void acquire_finish(struct flame_acquire *acquire);

#endif
