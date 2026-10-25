#ifndef FLAME_CONFIG_WATCH_H
#define FLAME_CONFIG_WATCH_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

// flame.lua and settings.lua watched: saved with something new in them, flame
// reloads by itself. their folders are what's watched, not the files: an
// editor saves by writing a new file and renaming it over the old one

struct flame_server;

struct flame_config_watch {
	struct flame_server *server;
	int fd; // inotify, -1 for none
	struct wl_event_source *source, *settle;
	char init[512], settings[512]; // the files' paths
	// their folders, and whether they're watched yet: one that isn't there
	// has its parent watched until it is (a first config, ~/.config/flame
	// made after flame started)
	char folders[2][512];
	bool watched[2];
	// what the files held at the last load: a save that changes nothing
	// (or one flame made and reloaded for already) reloads nothing
	uint64_t loaded;
};

// never fails for good: without inotify there's still super+shift+r
void config_watch_init(struct flame_config_watch *watch, struct flame_server *server,
	const char *init_path);
void config_watch_finish(struct flame_config_watch *watch);
// a config was just loaded from the files as they are now
void config_watch_loaded(struct flame_config_watch *watch);

#endif
