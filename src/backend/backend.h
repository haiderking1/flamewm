#ifndef FLAME_BACKEND_BACKEND_H
#define FLAME_BACKEND_BACKEND_H

#include <stdbool.h>
#include <wayland-server-core.h>

struct wlr_output;

// hardware side: drm/libinput on a tty, or a window when nested
struct flame_backend {
	struct wlr_backend *wlr;
	struct wlr_session *session; // NULL when nested
	struct wl_list gpus; // backend_gpu.link

	struct {
		// a gpu says one of its displays changed: plugged, unplugged, or
		// a setting flipped in the display's own menu (freesync)
		struct wl_signal hotplug;
	} events;
};

bool backend_init(struct flame_backend *backend, struct wl_event_loop *loop);
bool backend_start(struct flame_backend *backend);
void backend_finish(struct flame_backend *backend);

bool backend_change_vt(struct flame_backend *backend, unsigned vt);

// whether output's display can do vrr right now, asked from the kernel:
// wlroots only reads it when the display gets plugged in, and turning
// freesync on in the display's menu isn't a plug. false when it can't
// tell (not a drm output)
bool backend_vrr_capable(struct wlr_output *output, bool *capable);

#endif
