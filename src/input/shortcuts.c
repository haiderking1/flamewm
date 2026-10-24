#include "input/shortcuts.h"

#include <wlr/types/wlr_keyboard_shortcuts_inhibit_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

#include "core/server.h"
#include "util/listener.h"

static void forget_active(struct flame_shortcuts *shortcuts) {
	shortcuts->active = NULL;
	listener_disconnect(&shortcuts->active_destroy);
}

static void forget_escaped(struct flame_shortcuts *shortcuts) {
	shortcuts->escaped = NULL;
	listener_disconnect(&shortcuts->escaped_destroy);
}

static void handle_active_destroy(struct wl_listener *listener, void *data) {
	struct flame_shortcuts *shortcuts = wl_container_of(listener, shortcuts, active_destroy);
	forget_active(shortcuts);
}

static void handle_escaped_destroy(struct wl_listener *listener, void *data) {
	struct flame_shortcuts *shortcuts = wl_container_of(listener, shortcuts, escaped_destroy);
	forget_escaped(shortcuts);
}

static void deactivate(struct flame_shortcuts *shortcuts) {
	struct wlr_keyboard_shortcuts_inhibitor_v1 *active = shortcuts->active;
	if (active) {
		forget_active(shortcuts);
		wlr_log(WLR_DEBUG, "shortcuts: flame's again");
		wlr_keyboard_shortcuts_inhibitor_v1_deactivate(active);
	}
}

// the inhibitor of the surface with the keyboard holds, unless super+escape
// let go of it while it still has it
static void refocus(struct flame_shortcuts *shortcuts) {
	struct wlr_seat *seat = shortcuts->server->input.seat;
	struct wlr_surface *surface = seat->keyboard_state.focused_surface;
	struct wlr_keyboard_shortcuts_inhibitor_v1 *wanted = NULL, *each;
	wl_list_for_each(each, &shortcuts->manager->inhibitors, link) {
		if (surface && each->surface == surface && each->seat == seat) {
			wanted = each;
		}
	}
	if (shortcuts->escaped && shortcuts->escaped != wanted) {
		forget_escaped(shortcuts); // gone elsewhere: coming back, it holds again
	}
	if (wanted == shortcuts->escaped) {
		wanted = NULL;
	}
	if (wanted == shortcuts->active) {
		return;
	}
	deactivate(shortcuts);
	if (wanted) {
		wlr_log(WLR_DEBUG, "shortcuts: an app has them");
		shortcuts->active = wanted;
		listener_connect(&shortcuts->active_destroy, &wanted->events.destroy,
			handle_active_destroy);
		wlr_keyboard_shortcuts_inhibitor_v1_activate(wanted);
	}
}

static void handle_focus_change(struct wl_listener *listener, void *data) {
	struct flame_shortcuts *shortcuts = wl_container_of(listener, shortcuts, focus_change);
	refocus(shortcuts);
}

static void handle_new_inhibitor(struct wl_listener *listener, void *data) {
	struct flame_shortcuts *shortcuts = wl_container_of(listener, shortcuts, new_inhibitor);
	refocus(shortcuts);
}

bool shortcuts_init(struct flame_shortcuts *shortcuts, struct flame_server *server) {
	shortcuts->server = server;
	wl_list_init(&shortcuts->active_destroy.link);
	wl_list_init(&shortcuts->escaped_destroy.link);
	shortcuts->manager = wlr_keyboard_shortcuts_inhibit_v1_create(server->display);
	if (!shortcuts->manager) {
		wlr_log(WLR_ERROR, "failed to create the shortcuts inhibitor");
		return false;
	}
	listener_connect(&shortcuts->new_inhibitor, &shortcuts->manager->events.new_inhibitor,
		handle_new_inhibitor);
	listener_connect(&shortcuts->focus_change,
		&server->input.seat->keyboard_state.events.focus_change, handle_focus_change);
	return true;
}

void shortcuts_finish(struct flame_shortcuts *shortcuts) {
	forget_active(shortcuts);
	forget_escaped(shortcuts);
	listener_disconnect(&shortcuts->new_inhibitor);
	listener_disconnect(&shortcuts->focus_change);
}

bool shortcuts_inhibited(const struct flame_shortcuts *shortcuts) {
	return shortcuts->active != NULL;
}

void shortcuts_escape(struct flame_shortcuts *shortcuts) {
	struct wlr_keyboard_shortcuts_inhibitor_v1 *active = shortcuts->active;
	if (!active) {
		return;
	}
	deactivate(shortcuts);
	shortcuts->escaped = active;
	listener_connect(&shortcuts->escaped_destroy, &active->events.destroy,
		handle_escaped_destroy);
}
