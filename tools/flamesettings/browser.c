#include "browser.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "image/image.h"

// thumbnails kept at most, the least recently shown go first. a 300x188
// one is 225 KiB
#define THUMBS_KEPT 300
#define ENTRIES_MAX 4000

enum thumb_state {
	THUMB_QUEUED,
	THUMB_MAKING,
	THUMB_MADE,
	THUMB_FAILED,
	THUMB_DROPPED, // its folder was left before it was made: asked again, it's queued again
};

struct thumb {
	struct thumb *next; // browser.thumbs
	struct thumb *queued; // browser.queue
	char *path;
	int width, height;
	unsigned generation; // the folder it was asked for in
	enum thumb_state state; // under lock
	cairo_surface_t *surface; // set with THUMB_MADE, under lock
};

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
	pthread_mutex_lock(&browser->lock);
	browser->generation++;
	pthread_mutex_unlock(&browser->lock);
	return true;
}

// ---- thumbnails ----

// the picture cut to cover width x height
static cairo_surface_t *make(const char *path, int width, int height) {
	struct image image;
	char why[96];
	if (!image_load(path, width, height, &image, why, sizeof(why))) {
		return NULL;
	}
	cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	cairo_surface_t *in = cairo_image_surface_create_for_data((unsigned char *)image.data,
		CAIRO_FORMAT_ARGB32, image.width, image.height, image.stride);
	if (cairo_surface_status(out) || cairo_surface_status(in)) {
		cairo_surface_destroy(out);
		cairo_surface_destroy(in);
		image_finish(&image);
		return NULL;
	}
	cairo_t *cr = cairo_create(out);
	double sx = (double)width / image.width, sy = (double)height / image.height;
	double s = sx > sy ? sx : sy;
	cairo_translate(cr, (width - image.width * s) / 2, (height - image.height * s) / 2);
	cairo_scale(cr, s, s);
	cairo_set_source_surface(cr, in, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_paint(cr);
	cairo_destroy(cr);
	cairo_surface_destroy(in);
	image_finish(&image);
	cairo_surface_flush(out);
	return out;
}

static void *worker(void *data) {
	struct browser *browser = data;
	pthread_mutex_lock(&browser->lock);
	while (!browser->stop) {
		struct thumb *thumb = browser->queue;
		if (!thumb) {
			pthread_cond_wait(&browser->wake, &browser->lock);
			continue;
		}
		browser->queue = thumb->queued;
		thumb->queued = NULL;
		if (thumb->generation != browser->generation) {
			thumb->state = THUMB_DROPPED; // left before it got here
			continue;
		}
		thumb->state = THUMB_MAKING;
		pthread_mutex_unlock(&browser->lock);
		cairo_surface_t *surface = make(thumb->path, thumb->width, thumb->height);
		pthread_mutex_lock(&browser->lock);
		thumb->surface = surface;
		thumb->state = surface ? THUMB_MADE : THUMB_FAILED;
		fui_wake(browser->ui);
	}
	pthread_mutex_unlock(&browser->lock);
	return NULL;
}

// at the end of the queue, under lock
static void enqueue(struct browser *browser, struct thumb *thumb) {
	thumb->state = THUMB_QUEUED;
	thumb->generation = browser->generation;
	struct thumb **at = &browser->queue;
	while (*at) {
		at = &(*at)->queued;
	}
	*at = thumb;
	pthread_cond_signal(&browser->wake);
}

// the least recently shown made ones go past THUMBS_KEPT, under lock
static void evict(struct browser *browser) {
	size_t kept = 0;
	struct thumb **at = &browser->thumbs;
	while (*at) {
		struct thumb *thumb = *at;
		bool done = thumb->state == THUMB_MADE || thumb->state == THUMB_FAILED
			|| thumb->state == THUMB_DROPPED;
		if (++kept > THUMBS_KEPT && done) {
			*at = thumb->next;
			if (thumb->surface) {
				cairo_surface_destroy(thumb->surface);
			}
			free(thumb->path);
			free(thumb);
			browser->thumbs_len--;
			continue;
		}
		at = &thumb->next;
	}
}

cairo_surface_t *browser_thumb(struct browser *browser, const char *path, int width,
		int height, bool *failed) {
	*failed = false;
	if (!browser->threads_up) {
		*failed = true;
		return NULL;
	}
	pthread_mutex_lock(&browser->lock);
	struct thumb **at = &browser->thumbs, *thumb = NULL;
	for (; *at; at = &(*at)->next) {
		if ((*at)->width == width && (*at)->height == height && !strcmp((*at)->path, path)) {
			thumb = *at;
			*at = thumb->next; // to the front: just shown
			break;
		}
	}
	if (!thumb) {
		thumb = calloc(1, sizeof(*thumb));
		if (!thumb || !(thumb->path = strdup(path))) {
			free(thumb);
			pthread_mutex_unlock(&browser->lock);
			*failed = true;
			return NULL;
		}
		thumb->width = width;
		thumb->height = height;
		browser->thumbs_len++;
		enqueue(browser, thumb);
	} else if (thumb->state == THUMB_DROPPED) {
		enqueue(browser, thumb);
	}
	thumb->next = browser->thumbs;
	browser->thumbs = thumb;
	cairo_surface_t *surface = thumb->state == THUMB_MADE ? thumb->surface : NULL;
	*failed = thumb->state == THUMB_FAILED;
	if (browser->thumbs_len > THUMBS_KEPT) {
		evict(browser);
	}
	pthread_mutex_unlock(&browser->lock);
	return surface;
}

void browser_init(struct browser *browser, struct fui *ui) {
	*browser = (struct browser){.ui = ui};
	pthread_mutex_init(&browser->lock, NULL);
	pthread_cond_init(&browser->wake, NULL);
	for (int i = 0; i < 2; i++) {
		if (!pthread_create(&browser->threads[browser->threads_up], NULL, worker, browser)) {
			browser->threads_up++;
		}
	}
}

void browser_finish(struct browser *browser) {
	pthread_mutex_lock(&browser->lock);
	browser->stop = true;
	pthread_cond_broadcast(&browser->wake);
	pthread_mutex_unlock(&browser->lock);
	for (int i = 0; i < browser->threads_up; i++) {
		pthread_join(browser->threads[i], NULL);
	}
	while (browser->thumbs) {
		struct thumb *thumb = browser->thumbs;
		browser->thumbs = thumb->next;
		if (thumb->surface) {
			cairo_surface_destroy(thumb->surface);
		}
		free(thumb->path);
		free(thumb);
	}
	entries_free(browser);
	pthread_mutex_destroy(&browser->lock);
	pthread_cond_destroy(&browser->wake);
}
