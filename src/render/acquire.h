#ifndef FLAME_RENDER_ACQUIRE_H
#define FLAME_RENDER_ACQUIRE_H

#include <wayland-server-core.h>

struct wlr_compositor;

// client buffers only get used once the gpu finished writing them. a commit
// whose dmabuf is still being rendered waits, the window keeps its last frame
// on screen meanwhile, instead of us sampling it half drawn or our whole frame
// stalling on one slow client. the same thing mutter and kwin do.
// explicit sync clients (wlr_linux_drm_syncobj_v1) wait for their acquire
// point to signal: drawing with it unsignalled makes our frame wait for the
// client's gpu work, and KMS miss the vblank for everyone
struct flame_acquire {
	struct wl_event_loop *loop;
	struct wl_listener new_surface;
};

void acquire_init(struct flame_acquire *acquire, struct wl_event_loop *loop,
	struct wlr_compositor *compositor);
void acquire_finish(struct flame_acquire *acquire);

#endif
