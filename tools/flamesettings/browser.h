#ifndef FLAMESETTINGS_BROWSER_H
#define FLAMESETTINGS_BROWSER_H

#include <cairo.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>

#include "../flameui/flameui.h"

// what the Wallpaper page shows: a folder's folders and pictures (their
// thumbnails come from fui_thumb)

struct browser_entry {
	char *name;
	bool folder;
};

struct browser {
	struct fui *ui;
	char dir[PATH_MAX]; // the folder open, no trailing slash
	struct browser_entry *entries; // folders first, each sorted by name
	size_t len;
};

void browser_init(struct browser *browser, struct fui *ui);
void browser_finish(struct browser *browser);

// the folder at dir listed, false (and the old one kept) when it can't be
// read
bool browser_open(struct browser *browser, const char *dir);

#endif
