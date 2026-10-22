#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "image/image.h"
#include "internal.h"

// thumbnails of picture files: worker threads read and shrink them (a
// folder of 4k photos never holds the window up), fui_wake when one's
// ready, the least recently shown dropped past THUMBS_KEPT

// a 320x200 one is 250 KiB
#define THUMBS_KEPT 300

enum thumb_state {
	THUMB_QUEUED,
	THUMB_MAKING,
	THUMB_MADE,
	THUMB_FAILED,
	THUMB_DROPPED, // forgotten before it was made: asked again, it's queued again
};

struct thumb {
	struct thumb *next; // fui_thumbs.thumbs
	struct thumb *queued; // fui_thumbs.queue
	char *path;
	int width, height;
	unsigned generation; // asked for before the last fui_thumbs_forget: not wanted
	enum thumb_state state; // under lock
	cairo_surface_t *surface; // set with THUMB_MADE, under lock
};

struct fui_thumbs {
	struct fui *ui;
	struct thumb *thumbs; // every one asked for, most recently shown first
	size_t len;
	unsigned generation;
	pthread_t threads[2];
	int threads_up;
	bool stop;
	pthread_mutex_t lock;
	pthread_cond_t wake;
	struct thumb *queue; // asked for, not started, under lock
};

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
	struct fui_thumbs *thumbs = data;
	pthread_mutex_lock(&thumbs->lock);
	while (!thumbs->stop) {
		struct thumb *thumb = thumbs->queue;
		if (!thumb) {
			pthread_cond_wait(&thumbs->wake, &thumbs->lock);
			continue;
		}
		thumbs->queue = thumb->queued;
		thumb->queued = NULL;
		if (thumb->generation != thumbs->generation) {
			thumb->state = THUMB_DROPPED; // left before it got here
			continue;
		}
		thumb->state = THUMB_MAKING;
		pthread_mutex_unlock(&thumbs->lock);
		cairo_surface_t *surface = make(thumb->path, thumb->width, thumb->height);
		pthread_mutex_lock(&thumbs->lock);
		thumb->surface = surface;
		thumb->state = surface ? THUMB_MADE : THUMB_FAILED;
		fui_wake(thumbs->ui);
	}
	pthread_mutex_unlock(&thumbs->lock);
	return NULL;
}

// at the end of the queue, under lock
static void enqueue(struct fui_thumbs *thumbs, struct thumb *thumb) {
	thumb->state = THUMB_QUEUED;
	thumb->generation = thumbs->generation;
	struct thumb **at = &thumbs->queue;
	while (*at) {
		at = &(*at)->queued;
	}
	*at = thumb;
	pthread_cond_signal(&thumbs->wake);
}

// the least recently shown made ones go past THUMBS_KEPT, under lock
static void evict(struct fui_thumbs *thumbs) {
	size_t kept = 0;
	struct thumb **at = &thumbs->thumbs;
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
			thumbs->len--;
			continue;
		}
		at = &thumb->next;
	}
}

static struct fui_thumbs *thumbs_of(struct fui *ui) {
	if (ui->thumbs) {
		return ui->thumbs;
	}
	struct fui_thumbs *thumbs = calloc(1, sizeof(*thumbs));
	if (!thumbs) {
		return NULL;
	}
	thumbs->ui = ui;
	pthread_mutex_init(&thumbs->lock, NULL);
	pthread_cond_init(&thumbs->wake, NULL);
	for (int i = 0; i < 2; i++) {
		if (!pthread_create(&thumbs->threads[thumbs->threads_up], NULL, worker, thumbs)) {
			thumbs->threads_up++;
		}
	}
	ui->thumbs = thumbs;
	return thumbs;
}

cairo_surface_t *fui_thumb(struct fui *ui, const char *path, int width, int height,
		bool *failed) {
	*failed = false;
	struct fui_thumbs *thumbs = thumbs_of(ui);
	if (!thumbs || !thumbs->threads_up) {
		*failed = true;
		return NULL;
	}
	pthread_mutex_lock(&thumbs->lock);
	struct thumb **at = &thumbs->thumbs, *thumb = NULL;
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
			pthread_mutex_unlock(&thumbs->lock);
			*failed = true;
			return NULL;
		}
		thumb->width = width;
		thumb->height = height;
		thumbs->len++;
		enqueue(thumbs, thumb);
	} else if (thumb->state == THUMB_DROPPED) {
		enqueue(thumbs, thumb);
	}
	thumb->next = thumbs->thumbs;
	thumbs->thumbs = thumb;
	cairo_surface_t *surface = thumb->state == THUMB_MADE ? thumb->surface : NULL;
	*failed = thumb->state == THUMB_FAILED;
	if (thumbs->len > THUMBS_KEPT) {
		evict(thumbs);
	}
	pthread_mutex_unlock(&thumbs->lock);
	return surface;
}


void fui_thumbs_forget(struct fui *ui) {
	struct fui_thumbs *thumbs = ui->thumbs;
	if (!thumbs) {
		return;
	}
	pthread_mutex_lock(&thumbs->lock);
	thumbs->generation++;
	pthread_mutex_unlock(&thumbs->lock);
}

void fui_thumbs_free(struct fui *ui) {
	struct fui_thumbs *thumbs = ui->thumbs;
	if (!thumbs) {
		return;
	}
	pthread_mutex_lock(&thumbs->lock);
	thumbs->stop = true;
	pthread_cond_broadcast(&thumbs->wake);
	pthread_mutex_unlock(&thumbs->lock);
	for (int i = 0; i < thumbs->threads_up; i++) {
		pthread_join(thumbs->threads[i], NULL);
	}
	while (thumbs->thumbs) {
		struct thumb *thumb = thumbs->thumbs;
		thumbs->thumbs = thumb->next;
		if (thumb->surface) {
			cairo_surface_destroy(thumb->surface);
		}
		free(thumb->path);
		free(thumb);
	}
	pthread_mutex_destroy(&thumbs->lock);
	pthread_cond_destroy(&thumbs->wake);
	free(thumbs);
	ui->thumbs = NULL;
}
