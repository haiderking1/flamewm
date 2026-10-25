#include "config/watch.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <wlr/util/log.h>

#include "config/config.h"
#include "core/server.h"

// editors write in steps (a temp file, a rename, a chmod): reload once
// they're done
#define SETTLE_MS 150

// what both files hold, as one number. a missing file is a value too
static uint64_t contents(const struct flame_config_watch *watch) {
	uint64_t hash = 0xcbf29ce484222325ull;
	const char *paths[] = {watch->settings, watch->init};
	for (int i = 0; i < 2; i++) {
		FILE *f = fopen(paths[i], "rb");
		hash = (hash ^ (uint64_t)(f != NULL)) * 0x100000001b3ull;
		if (!f) {
			continue;
		}
		unsigned char buf[4096];
		size_t n;
		while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
			for (size_t k = 0; k < n; k++) {
				hash = (hash ^ buf[k]) * 0x100000001b3ull;
			}
		}
		fclose(f);
	}
	return hash;
}

void config_watch_loaded(struct flame_config_watch *watch) {
	if (watch->server) {
		watch->loaded = contents(watch);
	}
}

static int handle_settle(void *data) {
	struct flame_config_watch *watch = data;
	if (contents(watch) != watch->loaded) {
		wlr_log(WLR_INFO, "config: saved with something new, reloading");
		server_reload_config(watch->server);
	}
	return 0;
}

static void watch_folder(struct flame_config_watch *watch, int i);

static const char *base(const char *path) {
	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

static int handle_inotify(int fd, uint32_t mask, void *data) {
	struct flame_config_watch *watch = data;
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	bool ours = false;
	for (;;) {
		ssize_t len = read(fd, buf, sizeof(buf));
		if (len <= 0) {
			break;
		}
		for (char *p = buf; p < buf + len;) {
			const struct inotify_event *event = (const struct inotify_event *)p;
			if (event->len && (!strcmp(event->name, base(watch->init))
					|| !strcmp(event->name, base(watch->settings)))) {
				ours = true;
			}
			// a folder showed up, maybe one of the ones on the way to a config
			// that wasn't there: watched once it is, what's in it counts
			for (int i = 0; event->len && (event->mask & IN_ISDIR) && i < 2; i++) {
				if (!watch->watched[i]) {
					watch_folder(watch, i);
					ours = ours || watch->watched[i];
				}
			}
			p += sizeof(*event) + event->len;
		}
	}
	if (ours) {
		wl_event_source_timer_update(watch->settle, SETTLE_MS);
	}
	return 0;
}

static void folder_of(const char *path, char *folder, size_t size) {
	snprintf(folder, size, "%s", path);
	char *slash = strrchr(folder, '/');
	if (!slash) {
		snprintf(folder, size, ".");
	} else if (slash == folder) {
		folder[1] = '\0';
	} else {
		*slash = '\0';
	}
}

// folder i watched, or while it isn't there, the closest folder above it
// that is, for the ones in between showing up
static void watch_folder(struct flame_config_watch *watch, int i) {
	const char *folder = watch->folders[i];
	if (inotify_add_watch(watch->fd, folder,
			IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE | IN_MOVED_FROM) >= 0) {
		watch->watched[i] = true;
		return;
	}
	char above[512];
	snprintf(above, sizeof(above), "%s", folder);
	while (errno == ENOENT && strcmp(above, "/") && strcmp(above, ".")) {
		char up[512];
		folder_of(above, up, sizeof(up));
		snprintf(above, sizeof(above), "%s", up);
		if (inotify_add_watch(watch->fd, above, IN_CREATE | IN_MOVED_TO | IN_ONLYDIR) >= 0) {
			// made while this was looking: no event's coming for it
			if (inotify_add_watch(watch->fd, folder, IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE
					| IN_DELETE | IN_MOVED_FROM) >= 0) {
				watch->watched[i] = true;
			}
			return;
		}
	}
	wlr_log(WLR_INFO, "config: not watching %s (%s), super+shift+r reloads", folder,
		strerror(errno));
}

void config_watch_init(struct flame_config_watch *watch, struct flame_server *server,
		const char *init_path) {
	memset(watch, 0, sizeof(*watch));
	watch->fd = -1;
	watch->server = server;
	snprintf(watch->init, sizeof(watch->init), "%s", init_path);
	snprintf(watch->settings, sizeof(watch->settings), "%s", config_settings_path());
	watch->loaded = contents(watch);

	struct wl_event_loop *loop = wl_display_get_event_loop(server->display);
	watch->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (watch->fd < 0) {
		wlr_log(WLR_INFO, "config: no inotify (%s), super+shift+r reloads", strerror(errno));
		return;
	}
	watch->settle = wl_event_loop_add_timer(loop, handle_settle, watch);
	watch->source = wl_event_loop_add_fd(loop, watch->fd, WL_EVENT_READABLE, handle_inotify,
		watch);
	if (!watch->settle || !watch->source) {
		config_watch_finish(watch);
		return;
	}
	// one folder, or two when settings.lua lives somewhere else
	folder_of(watch->init, watch->folders[0], sizeof(watch->folders[0]));
	folder_of(watch->settings, watch->folders[1], sizeof(watch->folders[1]));
	watch_folder(watch, 0);
	if (strcmp(watch->folders[1], watch->folders[0])) {
		watch_folder(watch, 1);
	} else {
		watch->watched[1] = true;
	}
}

void config_watch_finish(struct flame_config_watch *watch) {
	if (watch->source) {
		wl_event_source_remove(watch->source);
	}
	if (watch->settle) {
		wl_event_source_remove(watch->settle);
	}
	if (watch->fd >= 0) {
		close(watch->fd);
	}
	watch->source = watch->settle = NULL;
	watch->fd = -1;
	watch->server = NULL;
}
