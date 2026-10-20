#include "render/acquire.h"

#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <unistd.h>
#include <wlr/render/dmabuf.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/util/log.h>

#include "util/listener.h"

// one surface's commits, for as long as it lives
struct acquire_surface {
	struct flame_acquire *acquire;
	struct wlr_surface *surface;
	struct wl_listener client_commit;
	struct wl_listener destroy;
};

struct wait;

// a plane whose last writer hasn't finished
struct wait_plane {
	struct wait *wait;
	int fd; // ours, a dup
	struct wl_event_source *source;
};

// one held commit, waiting on its busy planes
struct wait {
	struct wlr_surface *surface;
	uint32_t seq;
	struct wait_plane planes[WLR_DMABUF_MAX_PLANES];
	int remaining;
	struct wl_listener surface_destroy;
};

static void plane_finish(struct wait_plane *plane) {
	if (plane->source) {
		wl_event_source_remove(plane->source);
		plane->source = NULL;
	}
	if (plane->fd >= 0) {
		close(plane->fd);
		plane->fd = -1;
	}
}

static void wait_free(struct wait *wait) {
	for (int i = 0; i < WLR_DMABUF_MAX_PLANES; i++) {
		plane_finish(&wait->planes[i]);
	}
	listener_disconnect(&wait->surface_destroy);
	free(wait);
}

static int handle_plane_ready(int fd, uint32_t mask, void *data) {
	struct wait_plane *plane = data;
	struct wait *wait = plane->wait;
	plane_finish(plane);
	if (--wait->remaining > 0) {
		return 0;
	}
	// every writer is done: the commit lands now, like it just came in
	struct wlr_surface *surface = wait->surface;
	uint32_t seq = wait->seq;
	wait_free(wait);
	wlr_surface_unlock_cached(surface, seq);
	return 0;
}

static void handle_wait_surface_destroy(struct wl_listener *listener, void *data) {
	// the held state goes with the surface, nothing to release
	struct wait *wait = wl_container_of(listener, wait, surface_destroy);
	wait_free(wait);
}

// a dmabuf plane is readable once nothing is writing it anymore. errors
// count as ready: better a possibly early frame than a stuck window
static bool plane_busy(int fd) {
	struct pollfd pfd = {.fd = fd, .events = POLLIN};
	return poll(&pfd, 1, 0) == 0;
}

static void handle_client_commit(struct wl_listener *listener, void *data) {
	struct acquire_surface *tracked = wl_container_of(listener, tracked, client_commit);
	struct wlr_surface *surface = tracked->surface;
	const struct wlr_surface_state *pending = &surface->pending;
	if (!(pending->committed & WLR_SURFACE_STATE_BUFFER) || !pending->buffer
			|| wlr_linux_drm_syncobj_v1_get_surface_state(surface)) {
		return;
	}
	struct wlr_dmabuf_attributes dmabuf;
	if (!wlr_buffer_get_dmabuf(pending->buffer, &dmabuf)) {
		return; // shm and friends were copied already
	}

	struct wait *wait = NULL;
	for (int i = 0; i < dmabuf.n_planes; i++) {
		bool seen = false;
		for (int j = 0; j < i; j++) {
			seen |= dmabuf.fd[j] == dmabuf.fd[i];
		}
		if (seen || !plane_busy(dmabuf.fd[i])) {
			continue;
		}
		if (!wait) {
			wait = calloc(1, sizeof(*wait));
			if (!wait) {
				return; // out of memory: show it as it is
			}
			for (int k = 0; k < WLR_DMABUF_MAX_PLANES; k++) {
				wait->planes[k].fd = -1;
			}
			wait->surface = surface;
			wl_list_init(&wait->surface_destroy.link);
		}
		// the buffer's fds go away with it, the wait needs its own
		struct wait_plane *plane = &wait->planes[i];
		plane->wait = wait;
		plane->fd = fcntl(dmabuf.fd[i], F_DUPFD_CLOEXEC, 0);
		if (plane->fd >= 0) {
			plane->source = wl_event_loop_add_fd(tracked->acquire->loop, plane->fd,
				WL_EVENT_READABLE, handle_plane_ready, plane);
		}
		if (!plane->source) {
			plane_finish(plane); // can't watch it, don't wait for it
			continue;
		}
		wait->remaining++;
	}
	if (!wait) {
		return;
	}
	if (wait->remaining == 0) {
		wait_free(wait);
		return;
	}
	wait->seq = wlr_surface_lock_pending(surface);
	listener_connect(&wait->surface_destroy, &surface->events.destroy,
		handle_wait_surface_destroy);
}

static void handle_surface_destroy(struct wl_listener *listener, void *data) {
	struct acquire_surface *tracked = wl_container_of(listener, tracked, destroy);
	listener_disconnect(&tracked->client_commit);
	listener_disconnect(&tracked->destroy);
	free(tracked);
}

static void handle_new_surface(struct wl_listener *listener, void *data) {
	struct flame_acquire *acquire = wl_container_of(listener, acquire, new_surface);
	struct wlr_surface *surface = data;
	struct acquire_surface *tracked = calloc(1, sizeof(*tracked));
	if (!tracked) {
		wlr_log(WLR_ERROR, "out of memory");
		return;
	}
	tracked->acquire = acquire;
	tracked->surface = surface;
	listener_connect(&tracked->client_commit, &surface->events.client_commit,
		handle_client_commit);
	listener_connect(&tracked->destroy, &surface->events.destroy, handle_surface_destroy);
}

void acquire_init(struct flame_acquire *acquire, struct wl_event_loop *loop,
		struct wlr_compositor *compositor) {
	acquire->loop = loop;
	listener_connect(&acquire->new_surface, &compositor->events.new_surface,
		handle_new_surface);
}

void acquire_finish(struct flame_acquire *acquire) {
	listener_disconnect(&acquire->new_surface);
}
