#include "idle/idle.h"

#include <stdlib.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/util/log.h>

#include "core/server.h"
#include "scene/surface.h"
#include "util/listener.h"

#define RECHECK_MS 1000 // how often inhibitors are looked at while there are any
#define SETTLE_MS 100 // after a change: once the frame showing it is out

struct idle_inhibitor {
	struct wl_list link; // flame_idle.inhibitors
	struct flame_idle *idle;
	struct wlr_idle_inhibitor_v1 *wlr;
	struct wl_listener destroy;
};

// on screen: in the last frame flame drew. one on a workspace that isn't
// showing, or minimized under a fullscreen one, keeps nothing on. a still
// screen draws no new frames: one that went to a hidden workspace isn't in
// the last one, however long ago that was
static bool on_screen(struct flame_idle *idle, struct wlr_surface *surface) {
	struct flame_surface_state *state = scene_surface_state(surface);
	return surface->mapped && state && scene_surface_drawn(state);
}

static void update(struct flame_idle *idle, int next_ms) {
	bool inhibited = idle->x_suspended;
	struct idle_inhibitor *inhibitor;
	wl_list_for_each(inhibitor, &idle->inhibitors, link) {
		inhibited = inhibited || on_screen(idle, inhibitor->wlr->surface);
	}
	if (inhibited != idle->inhibited) {
		idle->inhibited = inhibited;
		wlr_log(WLR_DEBUG, "idle: %s", inhibited ? "kept from going idle"
			: "may go idle again");
		wlr_idle_notifier_v1_set_inhibited(idle->notifier, inhibited);
	}
	wl_event_source_timer_update(idle->recheck, wl_list_empty(&idle->inhibitors) ? 0 : next_ms);
}

static int handle_recheck(void *data) {
	update(data, RECHECK_MS);
	return 0;
}

static void handle_inhibitor_destroy(struct wl_listener *listener, void *data) {
	struct idle_inhibitor *inhibitor = wl_container_of(listener, inhibitor, destroy);
	struct flame_idle *idle = inhibitor->idle;
	listener_disconnect(&inhibitor->destroy);
	wl_list_remove(&inhibitor->link);
	free(inhibitor);
	update(idle, SETTLE_MS);
}

static void handle_new_inhibitor(struct wl_listener *listener, void *data) {
	struct flame_idle *idle = wl_container_of(listener, idle, new_inhibitor);
	struct idle_inhibitor *inhibitor = calloc(1, sizeof(*inhibitor));
	if (!inhibitor) {
		return;
	}
	inhibitor->idle = idle;
	inhibitor->wlr = data;
	listener_connect(&inhibitor->destroy, &inhibitor->wlr->events.destroy,
		handle_inhibitor_destroy);
	wl_list_insert(&idle->inhibitors, &inhibitor->link);
	update(idle, SETTLE_MS); // its window may only be about to show
}

static void handle_desktop_changed(struct wl_listener *listener, void *data) {
	struct flame_idle *idle = wl_container_of(listener, idle, desktop_changed);
	update(idle, SETTLE_MS); // windows came, went or moved: what shows now
}

bool idle_init(struct flame_idle *idle, struct flame_server *server) {
	idle->server = server;
	wl_list_init(&idle->inhibitors);
	idle->notifier = wlr_idle_notifier_v1_create(server->display);
	idle->inhibit_manager = wlr_idle_inhibit_v1_create(server->display);
	idle->recheck = wl_event_loop_add_timer(server->loop, handle_recheck, idle);
	if (!idle->notifier || !idle->inhibit_manager || !idle->recheck) {
		wlr_log(WLR_ERROR, "failed to set up idle");
		return false;
	}
	listener_connect(&idle->new_inhibitor, &idle->inhibit_manager->events.new_inhibitor,
		handle_new_inhibitor);
	listener_connect(&idle->desktop_changed, &server->desktop.events.changed,
		handle_desktop_changed);
	return true;
}

void idle_finish(struct flame_idle *idle) {
	struct idle_inhibitor *inhibitor, *tmp;
	wl_list_for_each_safe(inhibitor, tmp, &idle->inhibitors, link) {
		listener_disconnect(&inhibitor->destroy);
		wl_list_remove(&inhibitor->link);
		free(inhibitor);
	}
	listener_disconnect(&idle->new_inhibitor);
	listener_disconnect(&idle->desktop_changed);
	if (idle->recheck) {
		wl_event_source_remove(idle->recheck);
		idle->recheck = NULL;
	}
}

void idle_activity(struct flame_idle *idle) {
	if (idle->notifier) {
		wlr_idle_notifier_v1_notify_activity(idle->notifier, idle->server->input.seat);
	}
}

void idle_x_suspended(struct flame_idle *idle, bool suspended) {
	idle->x_suspended = suspended;
	update(idle, RECHECK_MS);
}
