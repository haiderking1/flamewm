// syscall (pidfd_open) isn't posix
#define _DEFAULT_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <systemd/sd-bus.h>
#include <unistd.h>
#include <wlr/util/log.h>

#include "core/server.h"
#include "portal/portal.h"
#include "util/spawn.h"

// files, both ways apps ask the desktop for them, answered by flamefiles:
//   org.freedesktop.impl.portal.FileChooser: an app's open and save dialogs
//     (through xdg-desktop-portal): flamefiles as a chooser, what it
//     prints the answer
//   org.freedesktop.FileManager1: "show in folder" (browsers, chat apps):
//     flamefiles at the folder, the file picked in it

#define MANAGER_NAME "org.freedesktop.FileManager1"
#define MANAGER_PATH "/org/freedesktop/FileManager1"
#define ARGS 24

enum response { RESPONSE_OK = 0, RESPONSE_CANCELLED = 1 };

// ---- paths and uris ----

// file:///a%20b into /a b; false for one that isn't a local file
static bool uri_path(const char *uri, char *out, size_t size) {
	if (!strncmp(uri, "file://", 7)) {
		uri += 7;
		if (!strncmp(uri, "localhost/", 10)) {
			uri += 9;
		}
	} else if (uri[0] != '/') {
		return false;
	}
	size_t n = 0;
	for (const char *c = uri; *c && n + 1 < size; c++) {
		unsigned hex;
		if (*c == '%' && sscanf(c + 1, "%2x", &hex) == 1) {
			out[n++] = (char)hex;
			c += 2;
		} else {
			out[n++] = *c;
		}
	}
	out[n] = '\0';
	return n > 0;
}

// /a b into file:///a%20b
static char *path_uri(const char *path) {
	size_t len = strlen(path);
	char *uri = malloc(7 + len * 3 + 1);
	if (!uri) {
		return NULL;
	}
	char *at = uri + sprintf(uri, "file://");
	for (const unsigned char *c = (const unsigned char *)path; *c; c++) {
		bool plain = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z')
			|| (*c >= '0' && *c <= '9') || strchr("-._~/", *c);
		at += plain ? sprintf(at, "%c", *c) : sprintf(at, "%%%02X", *c);
	}
	return uri;
}

// ---- a dialog ----

struct chooser {
	struct wl_list link; // flame_portal.choosers
	struct flame_portal *portal;
	sd_bus_message *call; // what's answered when it's done
	sd_bus_slot *request; // the app taking it back (Close)
	pid_t pid;
	int out, pidfd;
	struct wl_event_source *out_source, *exit_source;
	char *got; // what flamefiles printed so far
	size_t len, cap;
	// SaveFiles: the names that go in the folder picked
	char **names;
	size_t names_len;
};

static void chooser_free(struct chooser *c) {
	if (c->out_source) {
		wl_event_source_remove(c->out_source);
	}
	if (c->exit_source) {
		wl_event_source_remove(c->exit_source);
	}
	if (c->out >= 0) {
		close(c->out);
	}
	if (c->pidfd >= 0) {
		close(c->pidfd);
	}
	sd_bus_slot_unref(c->request);
	sd_bus_message_unref(c->call);
	for (size_t i = 0; i < c->names_len; i++) {
		free(c->names[i]);
	}
	free(c->names);
	free(c->got);
	wl_list_remove(&c->link);
	free(c);
}

static void take_output(struct chooser *c) {
	char chunk[4096];
	ssize_t n;
	while (c->out >= 0 && (n = read(c->out, chunk, sizeof(chunk))) != 0) {
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			if (errno == EAGAIN) {
				return;
			}
			break;
		}
		if (c->len + (size_t)n + 1 > c->cap) {
			size_t cap = (c->len + (size_t)n + 1) * 2;
			char *grown = realloc(c->got, cap);
			if (!grown) {
				break;
			}
			c->got = grown;
			c->cap = cap;
		}
		memcpy(c->got + c->len, chunk, (size_t)n);
		c->len += (size_t)n;
		c->got[c->len] = '\0';
	}
	// all of it, or it broke: nothing more from it
	if (c->out_source) {
		wl_event_source_remove(c->out_source);
		c->out_source = NULL;
	}
	if (c->out >= 0) {
		close(c->out);
		c->out = -1;
	}
}

// the uris of what was picked (a line each), or of the names in the
// folder picked (SaveFiles), into reply's array
static int append_uris(struct chooser *c, sd_bus_message *reply, size_t *count) {
	int r = sd_bus_message_open_container(reply, 'a', "s");
	char *save = NULL;
	for (char *line = c->got ? strtok_r(c->got, "\n", &save) : NULL; r >= 0 && line;
			line = strtok_r(NULL, "\n", &save)) {
		for (size_t i = 0; i < (c->names_len ? c->names_len : 1) && r >= 0; i++) {
			char path[4096];
			if (c->names_len) {
				snprintf(path, sizeof(path), "%s/%s", line, c->names[i]);
			} else {
				snprintf(path, sizeof(path), "%s", line);
			}
			char *uri = path_uri(path);
			if (uri) {
				r = sd_bus_message_append(reply, "s", uri);
				free(uri);
				(*count)++;
			}
		}
		if (c->names_len) {
			break; // one folder
		}
	}
	return r < 0 ? r : sd_bus_message_close_container(reply);
}

static void answer(struct chooser *c) {
	sd_bus_message *reply = NULL;
	size_t count = 0;
	// what's picked, or nothing for cancelled
	if (sd_bus_message_new_method_return(c->call, &reply) >= 0) {
		bool picked = c->got && c->len > 0;
		int r = sd_bus_message_append(reply, "u",
			(uint32_t)(picked ? RESPONSE_OK : RESPONSE_CANCELLED));
		r = r < 0 ? r : sd_bus_message_open_container(reply, 'a', "{sv}");
		if (r >= 0 && picked) {
			r = sd_bus_message_open_container(reply, 'e', "sv");
			r = r < 0 ? r : sd_bus_message_append(reply, "s", "uris");
			r = r < 0 ? r : sd_bus_message_open_container(reply, 'v', "as");
			r = r < 0 ? r : append_uris(c, reply, &count);
			r = r < 0 ? r : sd_bus_message_close_container(reply);
			r = r < 0 ? r : sd_bus_message_close_container(reply);
		}
		r = r < 0 ? r : sd_bus_message_close_container(reply);
		if (r >= 0) {
			sd_bus_send(NULL, reply, NULL);
		}
		sd_bus_message_unref(reply);
	}
	wlr_log(WLR_DEBUG, "portal: a file dialog done, %zu picked", count);
	struct flame_portal *portal = c->portal;
	chooser_free(c);
	portal_pump(portal);
}

static int handle_out(int fd, uint32_t mask, void *data) {
	take_output(data);
	return 0;
}

// flamefiles is gone: what it printed is the answer
static int handle_exit(int fd, uint32_t mask, void *data) {
	struct chooser *c = data;
	take_output(c);
	waitpid(c->pid, NULL, 0);
	answer(c);
	return 0;
}

static int method_close(sd_bus_message *m, void *data, sd_bus_error *error) {
	struct chooser *c = data;
	kill(c->pid, SIGTERM); // the app took it back: the dialog goes, it's answered then
	return sd_bus_reply_method_return(m, "");
}

static const sd_bus_vtable request_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Close", "", "", method_close, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_VTABLE_END,
};

// ---- what the app asks for ----

struct ask {
	const char *args[ARGS];
	int len;
	char accept[1024], folder[4096], name[256], title[256], label[64];
	bool multiple, directory;
	char **names; // SaveFiles'
	size_t names_len;
};

static void arg(struct ask *ask, const char *value) {
	if (ask->len < ARGS - 1) {
		ask->args[ask->len++] = value;
	}
}

// a byte string option (a path, nul ended)
static int read_bytes(sd_bus_message *m, char *out, size_t size) {
	const void *data;
	size_t len;
	int r = sd_bus_message_enter_container(m, 'v', "ay");
	r = r < 0 ? r : sd_bus_message_read_array(m, 'y', &data, &len);
	if (r >= 0) {
		len = len < size ? len : size - 1;
		memcpy(out, data, len);
		out[len] = '\0';
	}
	return r < 0 ? r : sd_bus_message_exit_container(m);
}

// a filter (name, then patterns: 0 a glob, 1 a mime type) into accept:
// only the first one read is kept (the current one's read first)
static int read_filter(sd_bus_message *m, char *accept, size_t size, bool keep) {
	const char *name;
	int r = sd_bus_message_enter_container(m, 'r', "sa(us)");
	r = r < 0 ? r : sd_bus_message_read(m, "s", &name);
	r = r < 0 ? r : sd_bus_message_enter_container(m, 'a', "(us)");
	uint32_t kind;
	const char *pattern;
	while (r >= 0 && (r = sd_bus_message_read(m, "(us)", &kind, &pattern)) > 0) {
		if (keep && strlen(accept) + strlen(pattern) + 2 < size) {
			if (*accept) {
				strcat(accept, ";");
			}
			strcat(accept, pattern);
		}
	}
	r = r < 0 ? r : sd_bus_message_exit_container(m);
	return r < 0 ? r : sd_bus_message_exit_container(m);
}

static int read_options(sd_bus_message *m, struct ask *ask) {
	int r = sd_bus_message_enter_container(m, 'a', "{sv}");
	bool have_filter = false;
	while (r >= 0 && (r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		const char *key;
		if ((r = sd_bus_message_read(m, "s", &key)) < 0) {
			return r;
		}
		int flag;
		const char *text;
		if (!strcmp(key, "multiple") && (r = sd_bus_message_read(m, "v", "b", &flag)) >= 0) {
			ask->multiple = flag;
		} else if (!strcmp(key, "directory")
				&& (r = sd_bus_message_read(m, "v", "b", &flag)) >= 0) {
			ask->directory = flag;
		} else if (!strcmp(key, "accept_label")
				&& (r = sd_bus_message_read(m, "v", "s", &text)) >= 0) {
			snprintf(ask->label, sizeof(ask->label), "%s", text);
		} else if (!strcmp(key, "current_name")
				&& (r = sd_bus_message_read(m, "v", "s", &text)) >= 0) {
			snprintf(ask->name, sizeof(ask->name), "%s", text);
		} else if (!strcmp(key, "current_folder")) {
			r = read_bytes(m, ask->folder, sizeof(ask->folder));
		} else if (!strcmp(key, "current_file")) {
			// saving over one: its folder, its name
			char file[4096];
			r = read_bytes(m, file, sizeof(file));
			char *slash = strrchr(file, '/');
			if (r >= 0 && slash) {
				snprintf(ask->name, sizeof(ask->name), "%s", slash + 1);
				*slash = '\0';
				snprintf(ask->folder, sizeof(ask->folder), "%s", *file ? file : "/");
			}
		} else if (!strcmp(key, "current_filter")) {
			ask->accept[0] = '\0';
			r = sd_bus_message_enter_container(m, 'v', "(sa(us))");
			r = r < 0 ? r : read_filter(m, ask->accept, sizeof(ask->accept), true);
			r = r < 0 ? r : sd_bus_message_exit_container(m);
			have_filter = true;
		} else if (!strcmp(key, "filters")) {
			r = sd_bus_message_enter_container(m, 'v', "a(sa(us))");
			r = r < 0 ? r : sd_bus_message_enter_container(m, 'a', "(sa(us))");
			bool first = true;
			while (r >= 0 && (r = sd_bus_message_peek_type(m, NULL, NULL)) > 0) {
				// no current one said: the first's what it's after
				r = read_filter(m, ask->accept, sizeof(ask->accept), first && !have_filter);
				first = false;
			}
			r = r < 0 ? r : sd_bus_message_exit_container(m);
			r = r < 0 ? r : sd_bus_message_exit_container(m);
			have_filter = have_filter || !first;
		} else if (!strcmp(key, "files")) {
			// SaveFiles: their names
			r = sd_bus_message_enter_container(m, 'v', "aay");
			r = r < 0 ? r : sd_bus_message_enter_container(m, 'a', "ay");
			const void *data;
			size_t len;
			while (r >= 0 && (r = sd_bus_message_read_array(m, 'y', &data, &len)) > 0) {
				char **grown = realloc(ask->names, (ask->names_len + 1) * sizeof(*grown));
				if (!grown) {
					break;
				}
				ask->names = grown;
				char *name = strndup(data, len);
				if (!name) {
					break;
				}
				char *slash = strrchr(name, '/');
				memmove(name, slash ? slash + 1 : name, strlen(slash ? slash + 1 : name) + 1);
				ask->names[ask->names_len++] = name;
			}
			r = r < 0 ? r : sd_bus_message_exit_container(m);
			r = r < 0 ? r : sd_bus_message_exit_container(m);
		} else {
			r = sd_bus_message_skip(m, "v");
		}
		if (r >= 0) {
			r = sd_bus_message_exit_container(m);
		}
	}
	return r < 0 ? r : sd_bus_message_exit_container(m);
}

enum dialog { OPEN, SAVE, SAVE_MANY };

static int dialog(sd_bus_message *m, struct flame_portal *portal, enum dialog kind) {
	const char *handle, *app_id, *parent, *title;
	int r = sd_bus_message_read(m, "osss", &handle, &app_id, &parent, &title);
	if (r < 0) {
		return r;
	}
	struct ask ask = {0};
	snprintf(ask.title, sizeof(ask.title), "%s", title);
	if ((r = read_options(m, &ask)) < 0) {
		for (size_t i = 0; i < ask.names_len; i++) {
			free(ask.names[i]);
		}
		free(ask.names);
		return r;
	}
	arg(&ask, "flamefiles");
	if (kind == SAVE) {
		arg(&ask, "--save");
		arg(&ask, *ask.name ? ask.name : "Untitled");
	} else if (kind == SAVE_MANY || ask.directory) {
		arg(&ask, "--pick-folder");
	} else {
		arg(&ask, "--pick");
		if (ask.multiple) {
			arg(&ask, "--multiple");
		}
		if (*ask.accept) {
			arg(&ask, "--accept");
			arg(&ask, ask.accept);
		}
	}
	if (*ask.title) {
		arg(&ask, "--title");
		arg(&ask, ask.title);
	}
	if (*ask.label) {
		arg(&ask, "--label");
		arg(&ask, ask.label);
	}
	if (*ask.folder) {
		arg(&ask, ask.folder);
	}
	ask.args[ask.len] = NULL;

	struct chooser *c = calloc(1, sizeof(*c));
	if (!c) {
		return -ENOMEM;
	}
	wl_list_insert(&portal->choosers, &c->link);
	c->portal = portal;
	c->call = sd_bus_message_ref(m);
	c->names = ask.names;
	c->names_len = ask.names_len;
	c->out = c->pidfd = -1;
	c->pid = spawn_reading(ask.args, &c->out);
	c->pidfd = c->pid > 0 ? (int)syscall(SYS_pidfd_open, c->pid, 0) : -1;
	struct wl_event_loop *loop = portal->server->loop;
	if (c->pid <= 0 || c->pidfd < 0) {
		wlr_log(WLR_ERROR, "portal: no flamefiles for a file dialog");
		if (c->pid > 0) {
			kill(c->pid, SIGTERM);
			waitpid(c->pid, NULL, 0);
		}
		answer(c);
		return 1;
	}
	c->out_source = wl_event_loop_add_fd(loop, c->out, WL_EVENT_READABLE, handle_out, c);
	c->exit_source = wl_event_loop_add_fd(loop, c->pidfd, WL_EVENT_READABLE, handle_exit, c);
	sd_bus_add_object_vtable(portal->bus, &c->request, handle,
		"org.freedesktop.impl.portal.Request", request_vtable, c);
	wlr_log(WLR_DEBUG, "portal: a file dialog for %s", *app_id ? app_id : "an app");
	return 1;
}

static int method_open_file(sd_bus_message *m, void *data, sd_bus_error *error) {
	return dialog(m, data, OPEN);
}

static int method_save_file(sd_bus_message *m, void *data, sd_bus_error *error) {
	return dialog(m, data, SAVE);
}

static int method_save_files(sd_bus_message *m, void *data, sd_bus_error *error) {
	return dialog(m, data, SAVE_MANY);
}

static int property_version(sd_bus *bus, const char *path, const char *interface,
		const char *property, sd_bus_message *reply, void *data, sd_bus_error *error) {
	return sd_bus_message_append(reply, "u", (uint32_t)4);
}

static const sd_bus_vtable chooser_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("OpenFile", "osssa{sv}", "ua{sv}", method_open_file,
		SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("SaveFile", "osssa{sv}", "ua{sv}", method_save_file,
		SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("SaveFiles", "osssa{sv}", "ua{sv}", method_save_files,
		SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_PROPERTY("version", "u", property_version, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_VTABLE_END,
};

// ---- show in folder ----

// flamefiles at each one (a file: its folder, it picked in it), a few at most
static int show(sd_bus_message *m) {
	int r = sd_bus_message_enter_container(m, 'a', "s");
	const char *uri;
	int shown = 0;
	while (r >= 0 && (r = sd_bus_message_read(m, "s", &uri)) > 0) {
		char path[4096];
		if (shown < 4 && uri_path(uri, path, sizeof(path))) {
			const char *const argv[] = {"flamefiles", path, NULL};
			spawn(argv);
			shown++;
		}
	}
	return r < 0 ? r : sd_bus_reply_method_return(m, "");
}

static int method_show(sd_bus_message *m, void *data, sd_bus_error *error) {
	return show(m);
}

static const sd_bus_vtable manager_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("ShowFolders", "ass", "", method_show, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("ShowItems", "ass", "", method_show, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("ShowItemProperties", "ass", "", method_show, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_VTABLE_END,
};

// ---- setup ----

bool portal_files_init(struct flame_portal *portal) {
	wl_list_init(&portal->choosers);
	int r = sd_bus_add_object_vtable(portal->bus, &portal->files_slot, PORTAL_PATH,
		"org.freedesktop.impl.portal.FileChooser", chooser_vtable, portal);
	if (r < 0) {
		wlr_log(WLR_ERROR, "portal: no file dialogs: %s", strerror(-r));
		return false;
	}
	// show in folder: flame's, whoever had it (a file manager that's running
	// keeps it till it goes)
	if ((r = sd_bus_add_object_vtable(portal->bus, &portal->manager_slot, MANAGER_PATH,
			MANAGER_NAME, manager_vtable, portal)) < 0
			|| (r = sd_bus_request_name(portal->bus, MANAGER_NAME,
				SD_BUS_NAME_REPLACE_EXISTING | SD_BUS_NAME_QUEUE)) < 0) {
		wlr_log(WLR_ERROR, "portal: \"show in folder\" isn't flame's: %s", strerror(-r));
	}
	return true;
}

void portal_files_finish(struct flame_portal *portal) {
	if (!portal->choosers.next) {
		return; // never set up
	}
	struct chooser *c, *tmp;
	wl_list_for_each_safe(c, tmp, &portal->choosers, link) {
		kill(c->pid, SIGTERM);
		waitpid(c->pid, NULL, 0);
		chooser_free(c);
	}
	sd_bus_slot_unref(portal->manager_slot);
	sd_bus_slot_unref(portal->files_slot);
	portal->manager_slot = portal->files_slot = NULL;
}
