#ifndef FLAMESETTINGS_BROWSER_H
#define FLAMESETTINGS_BROWSER_H

#include <cairo.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

#include "../flameui/flameui.h"

// what the Wallpaper page shows: a folder's folders and pictures, and
// thumbnails of the pictures, made by worker threads (a folder of 4k
// photos never holds the window up) and kept for when they're back

struct browser_entry {
	char *name;
	bool folder;
};

struct thumb; // a picture's thumbnail, made or on its way

struct browser {
	struct fui *ui;
	char dir[PATH_MAX]; // the folder open, no trailing slash
	struct browser_entry *entries; // folders first, each sorted by name
	size_t len;
	unsigned generation; // bumped with every folder: older asks are dropped

	struct thumb *thumbs; // every one asked for, newest first
	size_t thumbs_len;

	pthread_t threads[2];
	int threads_up;
	bool stop;
	pthread_mutex_t lock;
	pthread_cond_t wake;
	struct thumb *queue; // asked for, not started, under lock
};

void browser_init(struct browser *browser, struct fui *ui);
void browser_finish(struct browser *browser);

// the folder at dir listed, false (and the old one kept) when it can't be
// read
bool browser_open(struct browser *browser, const char *dir);
// path's thumbnail, width x height pixels, cut to cover that. NULL while
// it's being made (asked for the first time this is called) or when the
// picture can't be read: failed says which
cairo_surface_t *browser_thumb(struct browser *browser, const char *path, int width,
	int height, bool *failed);

#endif
