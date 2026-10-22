#include "backend/backend.h"

#include <stdlib.h>
#include <string.h>
#include <wlr/backend.h>
#include <wlr/backend/drm.h>
#include <wlr/backend/session.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>
#include <xf86drmMode.h>

#include "util/listener.h"

// a gpu the session opened, its hotplug events go out as one signal
struct backend_gpu {
	struct wl_list link; // flame_backend.gpus
	struct flame_backend *backend;
	struct wl_listener change;
	struct wl_listener remove;
};

static void gpu_destroy(struct backend_gpu *gpu) {
	listener_disconnect(&gpu->change);
	listener_disconnect(&gpu->remove);
	wl_list_remove(&gpu->link);
	free(gpu);
}

static void handle_gpu_change(struct wl_listener *listener, void *data) {
	struct backend_gpu *gpu = wl_container_of(listener, gpu, change);
	const struct wlr_device_change_event *event = data;
	if (event->type == WLR_DEVICE_HOTPLUG) {
		wl_signal_emit_mutable(&gpu->backend->events.hotplug, NULL);
	}
}

static void handle_gpu_remove(struct wl_listener *listener, void *data) {
	struct backend_gpu *gpu = wl_container_of(listener, gpu, remove);
	gpu_destroy(gpu);
}

static void watch_gpus(struct flame_backend *backend) {
	struct wlr_device *dev;
	wl_list_for_each(dev, &backend->session->devices, link) {
		struct backend_gpu *gpu = calloc(1, sizeof(*gpu));
		if (!gpu) {
			wlr_log(WLR_ERROR, "out of memory");
			return;
		}
		gpu->backend = backend;
		listener_connect(&gpu->change, &dev->events.change, handle_gpu_change);
		// ahead of the drm backend's own: it closes the device on remove,
		// and wlroots asserts nobody listens for changes by then
		gpu->remove.notify = handle_gpu_remove;
		wl_list_insert(&dev->events.remove.listener_list, &gpu->remove.link);
		wl_list_insert(&backend->gpus, &gpu->link);
	}
}

bool backend_init(struct flame_backend *backend, struct wl_event_loop *loop) {
	wl_list_init(&backend->gpus);
	wl_signal_init(&backend->events.hotplug);
	backend->wlr = wlr_backend_autocreate(loop, &backend->session);
	if (!backend->wlr) {
		wlr_log(WLR_ERROR, "failed to create backend");
		return false;
	}
	// the gpus are open by now, input devices come with the start
	if (backend->session) {
		watch_gpus(backend);
	}
	return true;
}

bool backend_start(struct flame_backend *backend) {
	if (!wlr_backend_start(backend->wlr)) {
		wlr_log(WLR_ERROR, "failed to start backend");
		return false;
	}
	return true;
}

void backend_finish(struct flame_backend *backend) {
	struct backend_gpu *gpu, *tmp;
	wl_list_for_each_safe(gpu, tmp, &backend->gpus, link) {
		gpu_destroy(gpu);
	}
	wlr_backend_destroy(backend->wlr);
	backend->wlr = NULL;
	backend->session = NULL;
}

bool backend_change_vt(struct flame_backend *backend, unsigned vt) {
	if (!backend->session) {
		return false;
	}
	return wlr_session_change_vt(backend->session, vt);
}

bool backend_vrr_capable(struct wlr_output *output, bool *capable) {
	if (!wlr_output_is_drm(output)) {
		return false;
	}
	int fd = wlr_backend_get_drm_fd(output->backend);
	uint32_t id = wlr_drm_connector_get_id(output);
	drmModeObjectProperties *props = fd < 0 ? NULL
		: drmModeObjectGetProperties(fd, id, DRM_MODE_OBJECT_CONNECTOR);
	if (!props) {
		return false;
	}
	bool found = false;
	for (uint32_t i = 0; i < props->count_props && !found; i++) {
		drmModePropertyRes *prop = drmModeGetProperty(fd, props->props[i]);
		if (prop && !strcmp(prop->name, "vrr_capable")) {
			*capable = props->prop_values[i] != 0;
			found = true;
		}
		drmModeFreeProperty(prop);
	}
	drmModeFreeObjectProperties(props);
	return found;
}
