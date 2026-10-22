#define _GNU_SOURCE // mkstemps
#include "portal/portal.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

#include "core/server.h"
#include "desktop/view.h"
#include "screencast/screencast.h"

// org.freedesktop.impl.portal.ScreenCast, what xdg-desktop-portal calls for
// an app: CreateSession, SelectSources (what the app wants), Start (the user
// picks, the stream starts, its pipewire node goes back). a session lives
// until xdg-desktop-portal closes it, or its window or screen goes (Closed)

#define BUS_NAME "org.freedesktop.impl.portal.desktop.flamewm"
#define OBJECT_PATH "/org/freedesktop/portal/desktop"
#define RESTORE_VENDOR "flamewm"
#define RESTORE_VERSION 1

enum source_type {
	SOURCE_MONITOR = 1,
	SOURCE_WINDOW = 2,
};

enum cursor_mode {
	CURSOR_HIDDEN = 1,
	CURSOR_EMBEDDED = 2,
};

enum persist_mode {
	PERSIST_NONE = 0,
	PERSIST_TRANSIENT = 1, // while the app runs
};

enum response {
	RESPONSE_OK = 0,
	RESPONSE_CANCELLED = 1,
	RESPONSE_FAILED = 2,
};

// what a pick was, to find it again: a screen by its name, a window by its
// app and title
struct restore {
	bool has;
	uint32_t type; // enum source_type
	char *output, *app_id, *title;
};

struct portal_session {
	struct wl_list link; // flame_portal.sessions
	struct flame_portal *portal;
	char *path, *app_id;
	sd_bus_slot *slot;

	// what the app asked for
	uint32_t types, cursor_mode, persist_mode;
	struct restore restore;

	// Start, answered once the stream's up (or the user cancelled)
	sd_bus_message *start;
	char *request_path;
	sd_bus_slot *request_slot;
	bool asking; // the picker's open for it

	struct cast_stream *stream;
	struct restore picked; // what it shows
	struct wlr_box shown; // layout coords, the window's at 0, 0
	bool streaming; // Start answered with it

	// a Close: done from an idle, an object isn't freed inside its own call
	struct wl_event_source *later;
	bool cancel, close;
};

// who it's for in the log: an app with no id (a script calling the portal
// straight) has a blank one
static const char *who(const struct portal_session *session) {
	return *session->app_id ? session->app_id : "an app";
}

static void restore_clear(struct restore *restore) {
	free(restore->output);
	free(restore->app_id);
	free(restore->title);
	*restore = (struct restore){0};
}

// ---- the bus in flame's loop ----

static void pump(struct flame_portal *portal) {
	if (!portal->bus) {
		return;
	}
	while (sd_bus_process(portal->bus, NULL) > 0) {
	}
	int events = sd_bus_get_events(portal->bus);
	uint32_t mask = 0;
	if (events & POLLIN) {
		mask |= WL_EVENT_READABLE;
	}
	if (events & POLLOUT) {
		mask |= WL_EVENT_WRITABLE;
	}
	wl_event_source_fd_update(portal->source, mask);
	uint64_t until;
	if (sd_bus_get_timeout(portal->bus, &until) >= 0 && until != UINT64_MAX) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		uint64_t now_us = (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
		int ms = until > now_us ? (int)((until - now_us + 999) / 1000) : 1;
		wl_event_source_timer_update(portal->timer, ms > 0 ? ms : 1);
	} else {
		wl_event_source_timer_update(portal->timer, 0);
	}
}

static int handle_bus(int fd, uint32_t mask, void *data) {
	pump(data);
	return 0;
}

static int handle_timer(void *data) {
	pump(data);
	return 0;
}

// ---- sessions ----

static struct portal_session *session_find(struct flame_portal *portal, const char *path) {
	struct portal_session *session;
	wl_list_for_each(session, &portal->sessions, link) {
		if (!strcmp(session->path, path)) {
			return session;
		}
	}
	return NULL;
}

static void request_done(struct portal_session *session) {
	if (session->request_slot) {
		sd_bus_slot_unref(session->request_slot);
		session->request_slot = NULL;
	}
	free(session->request_path);
	session->request_path = NULL;
}

// Start answered with nothing to show: cancelled or failed
static void answer(struct portal_session *session, enum response response) {
	if (!session->start) {
		return;
	}
	sd_bus_reply_method_return(session->start, "ua{sv}", (uint32_t)response, 0);
	sd_bus_message_unref(session->start);
	session->start = NULL;
	request_done(session);
	pump(session->portal);
}

static void session_destroy(struct portal_session *session) {
	struct flame_portal *portal = session->portal;
	if (session->later) {
		wl_event_source_remove(session->later);
	}
	if (session->asking) {
		session->asking = false;
		picker_forget(&portal->server->picker, session);
	}
	answer(session, RESPONSE_CANCELLED);
	if (session->stream) {
		screencast_stream_destroy(session->stream);
	}
	sd_bus_slot_unref(session->slot);
	wl_list_remove(&session->link);
	restore_clear(&session->restore);
	restore_clear(&session->picked);
	free(session->path);
	free(session->app_id);
	free(session);
	pump(portal);
}

static void handle_later(void *data) {
	struct portal_session *session = data;
	session->later = NULL;
	if (session->close) {
		session_destroy(session);
		return;
	}
	if (session->cancel) {
		session->cancel = false;
		if (session->asking) {
			session->asking = false;
			picker_forget(&session->portal->server->picker, session);
		}
		answer(session, RESPONSE_CANCELLED);
	}
}

static void later(struct portal_session *session) {
	if (!session->later) {
		session->later = wl_event_loop_add_idle(session->portal->server->loop, handle_later,
			session);
	}
}

static int method_session_close(sd_bus_message *m, void *data, sd_bus_error *error) {
	struct portal_session *session = data;
	wlr_log(WLR_DEBUG, "portal: %s closes its share", who(session));
	session->close = true;
	later(session);
	return sd_bus_reply_method_return(m, "");
}

static int property_version(sd_bus *bus, const char *path, const char *interface,
		const char *property, sd_bus_message *reply, void *data, sd_bus_error *error) {
	return sd_bus_message_append(reply, "u", (uint32_t)1);
}

static const sd_bus_vtable session_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Close", "", "", method_session_close, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_SIGNAL("Closed", "", 0),
	SD_BUS_PROPERTY("version", "u", property_version, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_VTABLE_END,
};

// the dialog's request: closing it cancels the pick
static int method_request_close(sd_bus_message *m, void *data, sd_bus_error *error) {
	struct portal_session *session = data;
	session->cancel = true;
	later(session);
	return sd_bus_reply_method_return(m, "");
}

static const sd_bus_vtable request_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Close", "", "", method_request_close, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_VTABLE_END,
};

// ---- options ----

static int read_string(sd_bus_message *m, char **out) {
	const char *s;
	int r = sd_bus_message_read(m, "v", "s", &s);
	if (r >= 0) {
		free(*out);
		*out = strdup(s);
	}
	return r;
}

// restore_data (suv): ours if the vendor is, the data a{sv}
static int read_restore(sd_bus_message *m, struct restore *restore) {
	int r = sd_bus_message_enter_container(m, 'v', "(suv)");
	if (r <= 0) {
		return r < 0 ? sd_bus_message_skip(m, "v") : r;
	}
	const char *vendor;
	uint32_t version;
	if ((r = sd_bus_message_enter_container(m, 'r', "suv")) < 0
			|| (r = sd_bus_message_read(m, "su", &vendor, &version)) < 0) {
		return r;
	}
	bool ours = !strcmp(vendor, RESTORE_VENDOR) && version == RESTORE_VERSION;
	if (!ours || sd_bus_message_enter_container(m, 'v', "a{sv}") <= 0) {
		r = sd_bus_message_skip(m, "v");
	} else {
		restore_clear(restore);
		r = sd_bus_message_enter_container(m, 'a', "{sv}");
		while (r >= 0 && (r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
			const char *key;
			if ((r = sd_bus_message_read(m, "s", &key)) < 0) {
				break;
			}
			if (!strcmp(key, "type")) {
				r = sd_bus_message_read(m, "v", "u", &restore->type);
			} else if (!strcmp(key, "output")) {
				r = read_string(m, &restore->output);
			} else if (!strcmp(key, "app_id")) {
				r = read_string(m, &restore->app_id);
			} else if (!strcmp(key, "title")) {
				r = read_string(m, &restore->title);
			} else {
				r = sd_bus_message_skip(m, "v");
			}
			if (r >= 0) {
				r = sd_bus_message_exit_container(m);
			}
		}
		if (r >= 0) {
			r = sd_bus_message_exit_container(m); // a
		}
		if (r >= 0) {
			r = sd_bus_message_exit_container(m); // v
		}
		restore->has = r >= 0 && restore->type;
	}
	if (r >= 0) {
		r = sd_bus_message_exit_container(m); // r
	}
	if (r >= 0) {
		r = sd_bus_message_exit_container(m); // v
	}
	return r;
}

// SelectSources' options: what the app wants. anything else skipped
static int read_options(sd_bus_message *m, struct portal_session *session) {
	int r = sd_bus_message_enter_container(m, 'a', "{sv}");
	while (r >= 0 && (r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		const char *key;
		if ((r = sd_bus_message_read(m, "s", &key)) < 0) {
			return r;
		}
		if (!session) {
			r = sd_bus_message_skip(m, "v");
		} else if (!strcmp(key, "types")) {
			r = sd_bus_message_read(m, "v", "u", &session->types);
		} else if (!strcmp(key, "cursor_mode")) {
			r = sd_bus_message_read(m, "v", "u", &session->cursor_mode);
		} else if (!strcmp(key, "persist_mode")) {
			r = sd_bus_message_read(m, "v", "u", &session->persist_mode);
		} else if (!strcmp(key, "restore_data")) {
			r = read_restore(m, &session->restore);
		} else {
			r = sd_bus_message_skip(m, "v");
		}
		if (r >= 0) {
			r = sd_bus_message_exit_container(m);
		}
	}
	return r < 0 ? r : sd_bus_message_exit_container(m);
}

// ---- Start's answer ----

static void append_restore(sd_bus_message *reply, const struct restore *picked) {
	sd_bus_message_open_container(reply, 'e', "sv");
	sd_bus_message_append(reply, "s", "restore_data");
	sd_bus_message_open_container(reply, 'v', "(suv)");
	sd_bus_message_open_container(reply, 'r', "suv");
	sd_bus_message_append(reply, "su", RESTORE_VENDOR, (uint32_t)RESTORE_VERSION);
	sd_bus_message_open_container(reply, 'v', "a{sv}");
	sd_bus_message_open_container(reply, 'a', "{sv}");
	sd_bus_message_append(reply, "{sv}", "type", "u", picked->type);
	if (picked->output) {
		sd_bus_message_append(reply, "{sv}", "output", "s", picked->output);
	}
	if (picked->app_id) {
		sd_bus_message_append(reply, "{sv}", "app_id", "s", picked->app_id);
	}
	if (picked->title) {
		sd_bus_message_append(reply, "{sv}", "title", "s", picked->title);
	}
	sd_bus_message_close_container(reply); // a
	sd_bus_message_close_container(reply); // v
	sd_bus_message_close_container(reply); // r
	sd_bus_message_close_container(reply); // v
	sd_bus_message_close_container(reply); // e
}

// the stream, where it is and how big (layout units), the pick to restore
static void answer_streams(struct portal_session *session, uint32_t node) {
	sd_bus_message *reply = NULL;
	if (sd_bus_message_new_method_return(session->start, &reply) < 0) {
		answer(session, RESPONSE_FAILED);
		return;
	}
	uint32_t persist = session->persist_mode ? PERSIST_TRANSIENT : PERSIST_NONE;
	sd_bus_message_append(reply, "u", (uint32_t)RESPONSE_OK);
	sd_bus_message_open_container(reply, 'a', "{sv}");
	sd_bus_message_open_container(reply, 'e', "sv");
	sd_bus_message_append(reply, "s", "streams");
	sd_bus_message_open_container(reply, 'v', "a(ua{sv})");
	sd_bus_message_open_container(reply, 'a', "(ua{sv})");
	sd_bus_message_open_container(reply, 'r', "ua{sv}");
	sd_bus_message_append(reply, "u", node);
	sd_bus_message_open_container(reply, 'a', "{sv}");
	sd_bus_message_append(reply, "{sv}", "position", "(ii)", session->shown.x,
		session->shown.y);
	sd_bus_message_append(reply, "{sv}", "size", "(ii)", session->shown.width,
		session->shown.height);
	sd_bus_message_append(reply, "{sv}", "source_type", "u", session->picked.type);
	sd_bus_message_close_container(reply); // a
	sd_bus_message_close_container(reply); // r
	sd_bus_message_close_container(reply); // a
	sd_bus_message_close_container(reply); // v
	sd_bus_message_close_container(reply); // e
	sd_bus_message_append(reply, "{sv}", "persist_mode", "u", persist);
	if (persist) {
		append_restore(reply, &session->picked);
	}
	sd_bus_message_close_container(reply); // a
	sd_bus_send(NULL, reply, NULL);
	sd_bus_message_unref(reply);
	sd_bus_message_unref(session->start);
	session->start = NULL;
	session->streaming = true;
	request_done(session);
	pump(session->portal);
}

// ---- the stream ----

static void handle_ready(struct cast_stream *stream, uint32_t node, int width, int height,
		void *data) {
	struct portal_session *session = data;
	wlr_log(WLR_DEBUG, "portal: %s's share is node %u, %dx%d", who(session), node, width,
		height);
	answer_streams(session, node);
}

static void handle_ended(struct cast_stream *stream, void *data) {
	struct portal_session *session = data;
	wlr_log(WLR_DEBUG, "portal: %s's share ended", who(session));
	struct flame_portal *portal = session->portal;
	if (session->start) {
		answer(session, RESPONSE_FAILED);
	} else if (portal->bus) {
		sd_bus_emit_signal(portal->bus, session->path, "org.freedesktop.impl.portal.Session",
			"Closed", "");
	}
	session_destroy(session);
}

static const struct cast_stream_events stream_events = {
	.ready = handle_ready,
	.ended = handle_ended,
};

static void remember(struct portal_session *session, const struct picker_pick *pick) {
	restore_clear(&session->picked);
	if (pick->output) {
		session->picked.type = SOURCE_MONITOR;
		session->picked.output = strdup(pick->output->wlr->name);
		session->shown = pick->output->box;
	} else {
		const char *app_id = view_app_id(pick->view), *title = view_title(pick->view);
		session->picked.type = SOURCE_WINDOW;
		session->picked.app_id = app_id ? strdup(app_id) : NULL;
		session->picked.title = title ? strdup(title) : NULL;
		int width = 0, height = 0;
		pick->view->impl->content_size(pick->view, &width, &height);
		session->shown = (struct wlr_box){0, 0, width, height};
	}
	session->picked.has = true;
}

static void stream_start(struct portal_session *session, const struct picker_pick *pick) {
	struct flame_server *server = session->portal->server;
	remember(session, pick);
	struct cast_target target = {
		.output = pick->output,
		.view = pick->view,
		.cursor = pick->output && session->cursor_mode == CURSOR_EMBEDDED,
	};
	session->stream = screencast_stream_create(&server->screencast, &target, &stream_events,
		session);
	if (!session->stream) {
		answer(session, RESPONSE_FAILED);
	}
}

static void handle_picked(const struct picker_pick *pick, void *data) {
	struct portal_session *session = data;
	session->asking = false;
	if (!pick) {
		wlr_log(WLR_DEBUG, "portal: %s's share cancelled", who(session));
		answer(session, RESPONSE_CANCELLED);
		return;
	}
	stream_start(session, pick);
}

// what an earlier pick was, if it's still there: the same screen, the same
// app's window (its title too when it has more than one)
static bool restored(struct portal_session *session, struct picker_pick *pick) {
	struct restore *restore = &session->restore;
	struct flame_server *server = session->portal->server;
	if (!restore->has || !(restore->type & session->types)) {
		return false;
	}
	if (restore->type == SOURCE_MONITOR && restore->output) {
		struct flame_output *output;
		wl_list_for_each(output, &server->outputs.list, link) {
			if (output->wlr->enabled && !strcmp(output->wlr->name, restore->output)) {
				*pick = (struct picker_pick){.mode = PICKER_SCREEN, .output = output};
				return true;
			}
		}
		return false;
	}
	if (restore->type != SOURCE_WINDOW || !restore->app_id) {
		return false;
	}
	struct flame_view *same_app = NULL;
	struct flame_desktop *desktop = &server->desktop;
	for (int i = 0; i < FLAME_WORKSPACE_COUNT; i++) {
		struct wl_list *lists[2] = {&desktop->workspaces[i].views, &desktop->workspaces[i].floats};
		for (int l = 0; l < 2; l++) {
			struct flame_view *view;
			wl_list_for_each(view, lists[l], link) {
				const char *app_id = view_app_id(view), *title = view_title(view);
				if (!app_id || strcmp(app_id, restore->app_id)) {
					continue;
				}
				if (title && restore->title && !strcmp(title, restore->title)) {
					*pick = (struct picker_pick){.mode = PICKER_WINDOW, .view = view};
					return true;
				}
				same_app = same_app ? same_app : view;
			}
		}
	}
	if (same_app) {
		*pick = (struct picker_pick){.mode = PICKER_WINDOW, .view = same_app};
		return true;
	}
	return false;
}

// ---- ScreenCast ----

static int method_create_session(sd_bus_message *m, void *data, sd_bus_error *error) {
	struct flame_portal *portal = data;
	const char *handle, *path, *app_id;
	int r = sd_bus_message_read(m, "oos", &handle, &path, &app_id);
	if (r < 0 || (r = read_options(m, NULL)) < 0) {
		return r;
	}
	struct portal_session *session = calloc(1, sizeof(*session));
	if (!session) {
		return -ENOMEM;
	}
	session->portal = portal;
	session->path = strdup(path);
	session->app_id = strdup(app_id);
	session->types = SOURCE_MONITOR;
	session->cursor_mode = CURSOR_HIDDEN;
	if (!session->path || !session->app_id || sd_bus_add_object_vtable(portal->bus,
			&session->slot, path, "org.freedesktop.impl.portal.Session", session_vtable,
			session) < 0) {
		free(session->path);
		free(session->app_id);
		free(session);
		return sd_bus_reply_method_return(m, "ua{sv}", (uint32_t)RESPONSE_FAILED, 0);
	}
	wl_list_insert(&portal->sessions, &session->link);
	wlr_log(WLR_DEBUG, "portal: %s starts a share", who(session));
	return sd_bus_reply_method_return(m, "ua{sv}", (uint32_t)RESPONSE_OK, 0);
}

static int method_select_sources(sd_bus_message *m, void *data, sd_bus_error *error) {
	struct flame_portal *portal = data;
	const char *handle, *path, *app_id;
	int r = sd_bus_message_read(m, "oos", &handle, &path, &app_id);
	if (r < 0) {
		return r;
	}
	struct portal_session *session = session_find(portal, path);
	if ((r = read_options(m, session)) < 0) {
		return r;
	}
	if (!session) {
		return sd_bus_reply_method_return(m, "ua{sv}", (uint32_t)RESPONSE_FAILED, 0);
	}
	session->types &= SOURCE_MONITOR | SOURCE_WINDOW;
	session->types = session->types ? session->types : SOURCE_MONITOR;
	wlr_log(WLR_DEBUG, "portal: %s wants %s, cursor %s, %s", who(session),
		session->types == SOURCE_MONITOR ? "a screen" : session->types == SOURCE_WINDOW
		? "a window" : "a screen or a window", session->cursor_mode == CURSOR_EMBEDDED
		? "in it" : "hidden", session->restore.has ? "an earlier pick to restore"
		: session->persist_mode ? "keeping the pick" : "just this once");
	return sd_bus_reply_method_return(m, "ua{sv}", (uint32_t)RESPONSE_OK, 0);
}

static int method_start(sd_bus_message *m, void *data, sd_bus_error *error) {
	struct flame_portal *portal = data;
	const char *handle, *path, *app_id, *parent;
	int r = sd_bus_message_read(m, "ooss", &handle, &path, &app_id, &parent);
	if (r < 0 || (r = read_options(m, NULL)) < 0) {
		return r;
	}
	struct portal_session *session = session_find(portal, path);
	if (!session || session->start || session->stream) {
		return sd_bus_reply_method_return(m, "ua{sv}", (uint32_t)RESPONSE_FAILED, 0);
	}
	session->start = sd_bus_message_ref(m);
	session->request_path = strdup(handle);
	if (session->request_path) {
		sd_bus_add_object_vtable(portal->bus, &session->request_slot, handle,
			"org.freedesktop.impl.portal.Request", request_vtable, session);
	}
	struct picker_pick pick;
	if (restored(session, &pick)) {
		wlr_log(WLR_DEBUG, "portal: %s gets its earlier pick again, no picker",
			who(session));
		stream_start(session, &pick);
		return 1;
	}
	unsigned kinds = (session->types & SOURCE_MONITOR ? PICKER_SCREENS : 0)
		| (session->types & SOURCE_WINDOW ? PICKER_WINDOWS : 0);
	session->asking = true;
	picker_ask(&portal->server->picker, kinds, handle_picked, session);
	return 1; // answered once there's a pick and a stream
}

static int property_source_types(sd_bus *bus, const char *path, const char *interface,
		const char *property, sd_bus_message *reply, void *data, sd_bus_error *error) {
	return sd_bus_message_append(reply, "u", (uint32_t)(SOURCE_MONITOR | SOURCE_WINDOW));
}

static int property_cursor_modes(sd_bus *bus, const char *path, const char *interface,
		const char *property, sd_bus_message *reply, void *data, sd_bus_error *error) {
	return sd_bus_message_append(reply, "u", (uint32_t)(CURSOR_HIDDEN | CURSOR_EMBEDDED));
}

static int property_screencast_version(sd_bus *bus, const char *path, const char *interface,
		const char *property, sd_bus_message *reply, void *data, sd_bus_error *error) {
	return sd_bus_message_append(reply, "u", (uint32_t)5);
}

static const sd_bus_vtable screencast_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("CreateSession", "oosa{sv}", "ua{sv}", method_create_session,
		SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("SelectSources", "oosa{sv}", "ua{sv}", method_select_sources,
		SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("Start", "oossa{sv}", "ua{sv}", method_start, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_PROPERTY("AvailableSourceTypes", "u", property_source_types, 0,
		SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("AvailableCursorModes", "u", property_cursor_modes, 0,
		SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("version", "u", property_screencast_version, 0,
		SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_VTABLE_END,
};

// ---- Screenshot ----

// one screenshot asked for: answered once it's taken (or the pick cancelled)
struct portal_shot {
	struct flame_portal *portal;
	sd_bus_message *call;
	sd_bus_slot *request_slot;
	struct wl_event_source *later; // a Close, done from an idle
};

static void shot_free(struct portal_shot *shot) {
	if (shot->later) {
		wl_event_source_remove(shot->later);
	}
	if (shot->request_slot) {
		sd_bus_slot_unref(shot->request_slot);
	}
	sd_bus_message_unref(shot->call);
	free(shot);
}

// the png goes in /tmp, a name of its own; the app gets its uri
static void shoot(struct portal_shot *shot, const struct picker_pick *pick) {
	struct flame_portal *portal = shot->portal;
	char path[] = "/tmp/flamewm-screenshot-XXXXXX.png";
	int fd = pick ? mkstemps(path, 4) : -1;
	if (fd >= 0) {
		close(fd);
	}
	struct cast_target target = {.output = pick ? pick->output : NULL,
		.view = pick ? pick->view : NULL};
	if (fd >= 0 && screencast_shoot(&portal->server->screencast, &target, path)) {
		char uri[sizeof(path) + 8];
		snprintf(uri, sizeof(uri), "file://%s", path);
		wlr_log(WLR_DEBUG, "portal: a screenshot, %s", path);
		sd_bus_reply_method_return(shot->call, "ua{sv}", (uint32_t)RESPONSE_OK, 1, "uri", "s",
			uri);
	} else {
		if (fd >= 0) {
			unlink(path);
		}
		sd_bus_reply_method_return(shot->call, "ua{sv}",
			(uint32_t)(pick ? RESPONSE_FAILED : RESPONSE_CANCELLED), 0);
	}
	shot_free(shot);
	pump(portal);
}

static void handle_shot_picked(const struct picker_pick *pick, void *data) {
	shoot(data, pick);
}

static void handle_shot_later(void *data) {
	struct portal_shot *shot = data;
	shot->later = NULL;
	picker_forget(&shot->portal->server->picker, shot);
	shoot(shot, NULL);
}

static int method_shot_close(sd_bus_message *m, void *data, sd_bus_error *error) {
	struct portal_shot *shot = data;
	if (!shot->later) {
		shot->later = wl_event_loop_add_idle(shot->portal->server->loop, handle_shot_later,
			shot);
	}
	return sd_bus_reply_method_return(m, "");
}

static const sd_bus_vtable shot_request_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Close", "", "", method_shot_close, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_VTABLE_END,
};

// the screen the pointer's on
static struct flame_output *pointer_output(struct flame_server *server) {
	struct wlr_cursor *cursor = server->input.cursor.wlr;
	struct flame_output *output;
	wl_list_for_each(output, &server->outputs.list, link) {
		if (output->wlr->enabled && wlr_box_contains_point(&output->box, cursor->x, cursor->y)) {
			return output;
		}
	}
	return outputs_primary(&server->outputs);
}

static int method_screenshot(sd_bus_message *m, void *data, sd_bus_error *error) {
	struct flame_portal *portal = data;
	const char *handle, *app_id, *parent;
	int r = sd_bus_message_read(m, "oss", &handle, &app_id, &parent);
	if (r < 0) {
		return r;
	}
	int interactive = 0;
	r = sd_bus_message_enter_container(m, 'a', "{sv}");
	while (r >= 0 && (r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		const char *key;
		if ((r = sd_bus_message_read(m, "s", &key)) < 0) {
			return r;
		}
		r = !strcmp(key, "interactive") ? sd_bus_message_read(m, "v", "b", &interactive)
			: sd_bus_message_skip(m, "v");
		if (r >= 0) {
			r = sd_bus_message_exit_container(m);
		}
	}
	struct portal_shot *shot = calloc(1, sizeof(*shot));
	if (!shot) {
		return -ENOMEM;
	}
	shot->portal = portal;
	shot->call = sd_bus_message_ref(m);
	if (!interactive) {
		struct flame_output *output = pointer_output(portal->server);
		shoot(shot, output ? &(struct picker_pick){.mode = PICKER_SCREEN, .output = output}
			: NULL);
		return 1;
	}
	sd_bus_add_object_vtable(portal->bus, &shot->request_slot, handle,
		"org.freedesktop.impl.portal.Request", shot_request_vtable, shot);
	picker_ask(&portal->server->picker, 0, handle_shot_picked, shot);
	return 1;
}

static int property_screenshot_version(sd_bus *bus, const char *path, const char *interface,
		const char *property, sd_bus_message *reply, void *data, sd_bus_error *error) {
	return sd_bus_message_append(reply, "u", (uint32_t)2);
}

static const sd_bus_vtable screenshot_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Screenshot", "ossa{sv}", "ua{sv}", method_screenshot,
		SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_PROPERTY("version", "u", property_screenshot_version, 0,
		SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_VTABLE_END,
};

// ---- setup ----

static void drop_bus(struct flame_portal *portal) {
	if (portal->source) {
		wl_event_source_remove(portal->source);
		portal->source = NULL;
	}
	if (portal->timer) {
		wl_event_source_remove(portal->timer);
		portal->timer = NULL;
	}
	if (portal->slot) {
		sd_bus_slot_unref(portal->slot);
		portal->slot = NULL;
	}
	if (portal->shot_slot) {
		sd_bus_slot_unref(portal->shot_slot);
		portal->shot_slot = NULL;
	}
	if (portal->bus) {
		sd_bus_flush_close_unref(portal->bus);
		portal->bus = NULL;
	}
}

bool portal_init(struct flame_portal *portal, struct flame_server *server) {
	portal->server = server;
	wl_list_init(&portal->sessions);
	const char *env = getenv("FLAME_PORTAL");
	bool wanted = env ? strcmp(env, "0") != 0 : server->backend.session != NULL;
	if (!wanted) {
		wlr_log(WLR_INFO, "portal: off, flame isn't the session here (FLAME_PORTAL=1 for it)");
		return true;
	}
	int r = sd_bus_open_user(&portal->bus);
	if (r < 0) {
		wlr_log(WLR_ERROR, "portal: no session bus: %s", strerror(-r));
		portal->bus = NULL;
		return true; // flame works on without it
	}
	if ((r = sd_bus_add_object_vtable(portal->bus, &portal->slot, OBJECT_PATH,
			"org.freedesktop.impl.portal.ScreenCast", screencast_vtable, portal)) < 0
			|| (r = sd_bus_add_object_vtable(portal->bus, &portal->shot_slot, OBJECT_PATH,
				"org.freedesktop.impl.portal.Screenshot", screenshot_vtable, portal)) < 0
			|| (r = sd_bus_request_name(portal->bus, BUS_NAME, 0)) < 0) {
		wlr_log(WLR_ERROR, "portal: can't serve %s: %s", BUS_NAME, strerror(-r));
		drop_bus(portal);
		return true;
	}
	portal->source = wl_event_loop_add_fd(server->loop, sd_bus_get_fd(portal->bus),
		WL_EVENT_READABLE, handle_bus, portal);
	portal->timer = wl_event_loop_add_timer(server->loop, handle_timer, portal);
	if (!portal->source || !portal->timer) {
		drop_bus(portal);
		return true;
	}
	pump(portal);
	wlr_log(WLR_INFO, "portal: screen sharing and screenshots served as %s", BUS_NAME);
	return true;
}

void portal_finish(struct flame_portal *portal) {
	struct portal_session *session, *tmp;
	wl_list_for_each_safe(session, tmp, &portal->sessions, link) {
		session_destroy(session);
	}
	drop_bus(portal);
}
