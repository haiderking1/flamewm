#include "browser.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>


#define ENTRIES_MAX 4000

// ---- the folder ----

static bool picture_name(const char *name) {
	const char *dot = strrchr(name, '.');
	static const char *const exts[] = {".jpg", ".jpeg", ".png", ".webp"};
	for (size_t i = 0; dot && i < sizeof(exts) / sizeof(exts[0]); i++) {
		if (!strcasecmp(dot, exts[i])) {
			return true;
		}
	}
	return false;
}

// folders first, then by name the way people count: 2 before 10
static int compare(const void *a, const void *b) {
	const struct browser_entry *x = a, *y = b;
	if (x->folder != y->folder) {
		return x->folder ? -1 : 1;
	}
	const char *s = x->name, *t = y->name;
	while (*s && *t) {
		if (*s >= '0' && *s <= '9' && *t >= '0' && *t <= '9') {
			char *se, *te;
			unsigned long long m = strtoull(s, &se, 10), n = strtoull(t, &te, 10);
			if (m != n) {
				return m < n ? -1 : 1;
			}
			s = se;
			t = te;
			continue;
		}
		int c = strncasecmp(s, t, 1);
		if (c) {
			return c;
		}
		s++;
		t++;
	}
	return *s ? 1 : *t ? -1 : 0;
}

static void entries_free(struct browser *browser) {
	for (size_t i = 0; i < browser->len; i++) {
		free(browser->entries[i].name);
	}
	free(browser->entries);
	browser->entries = NULL;
	browser->len = 0;
}

bool browser_open(struct browser *browser, const char *dir) {
	DIR *d = opendir(dir);
	if (!d) {
		return false;
	}
	struct browser_entry *entries = NULL;
	size_t len = 0, cap = 0;
	struct dirent *e;
	while ((e = readdir(d)) && len < ENTRIES_MAX) {
		if (e->d_name[0] == '.') {
			continue; // hidden, and . and ..
		}
		char path[PATH_MAX];
		struct stat st;
		if (snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= (int)sizeof(path)
				|| stat(path, &st) < 0) {
			continue;
		}
		bool folder = S_ISDIR(st.st_mode);
		if (!folder && !(S_ISREG(st.st_mode) && picture_name(e->d_name))) {
			continue;
		}
		if (len == cap) {
			cap = cap ? cap * 2 : 64;
			struct browser_entry *grown = realloc(entries, cap * sizeof(*entries));
			if (!grown) {
				break;
			}
			entries = grown;
		}
		entries[len].name = strdup(e->d_name);
		entries[len].folder = folder;
		if (entries[len].name) {
			len++;
		}
	}
	closedir(d);
	qsort(entries, len, sizeof(*entries), compare);
	entries_free(browser);
	browser->entries = entries;
	browser->len = len;
	snprintf(browser->dir, sizeof(browser->dir), "%s", dir);
	size_t dl = strlen(browser->dir);
	while (dl > 1 && browser->dir[dl - 1] == '/') {
		browser->dir[--dl] = '\0';
	}
	fui_thumbs_forget(browser->ui); // the last folder's aren't wanted now
	return true;
}

void browser_init(struct browser *browser, struct fui *ui) {
	*browser = (struct browser){.ui = ui};
}

void browser_finish(struct browser *browser) {
	entries_free(browser);
}
