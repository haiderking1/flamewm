#include "wallpaper/wallpaper.h"

#include <cairo.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

#include "anim/anim.h"
#include "core/server.h"
#include "image/image.h"
#include "output/output.h"
#include "render/render.h"
#include "scene/scene.h"

#define FADE_MS 450

// one picture on a screen: its own tree, so it can fade
struct picture {
	struct flame_tree *tree;
	struct flame_buffer_node *node;
	struct wlr_texture *texture;
};

struct wallpaper_screen {
	struct wl_list link; // flame_wallpaper.screens
	struct flame_wallpaper *wallpaper;
	struct flame_output *output;
	uint64_t id; // jobs find it by this: it may be gone when they're done
	uint64_t waiting; // the job it shows when it's done, 0 for none
	int width, height; // the pixels its picture's made for, 0 for none yet
	struct wlr_box box; // layout coords
	// what it shows, and the one before it fading out under it
	struct picture now, old;
	struct flame_anim fade;
	// the mirror's: the color, now's picture over it
	struct flame_tree *mirror;
	struct flame_rect *mirror_color;
	struct flame_buffer_node *mirror_picture;
};

// a picture made for a screen, by the worker
struct wallpaper_job {
	struct wl_list link;
	uint64_t id, screen;
	char *path;
	enum flame_wallpaper_mode mode;
	float color[4];
	int width, height;
	// what came out: the screen's pixels, or why not
	cairo_surface_t *made;
	char error[128];
};

static void job_free(struct wallpaper_job *job) {
	if (job->made) {
		cairo_surface_destroy(job->made);
	}
	free(job->path);
	free(job);
}

// ---- the worker ----

// the picture laid on the screen the way mode says, the background color
// around it, scaled with a proper filter (not every 8th pixel of a photo)
static void make(struct wallpaper_job *job) {
	struct image image;
	char why[96];
	// a fill covers the screen, anything smaller would be stretched up
	if (!image_load(job->path, job->mode == FLAME_WALLPAPER_CENTER ? 0 : job->width,
			job->mode == FLAME_WALLPAPER_CENTER ? 0 : job->height, &image, why, sizeof(why))) {
		snprintf(job->error, sizeof(job->error), "%s", why);
		return;
	}
	cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_RGB24, job->width,
		job->height);
	cairo_surface_t *in = cairo_image_surface_create_for_data((unsigned char *)image.data,
		CAIRO_FORMAT_ARGB32, image.width, image.height, image.stride);
	if (cairo_surface_status(out) || cairo_surface_status(in)) {
		snprintf(job->error, sizeof(job->error), "out of memory");
		cairo_surface_destroy(out);
		cairo_surface_destroy(in);
		image_finish(&image);
		return;
	}
	cairo_t *cr = cairo_create(out);
	// premultiplied, and on an opaque screen
	cairo_set_source_rgb(cr, job->color[0], job->color[1], job->color[2]);
	cairo_paint(cr);
	double sx = (double)job->width / image.width, sy = (double)job->height / image.height;
	switch (job->mode) {
	case FLAME_WALLPAPER_FILL:
		sx = sy = sx > sy ? sx : sy;
		break;
	case FLAME_WALLPAPER_FIT:
		sx = sy = sx < sy ? sx : sy;
		break;
	case FLAME_WALLPAPER_CENTER:
		sx = sy = 1;
		break;
	case FLAME_WALLPAPER_STRETCH:
		break;
	}
	cairo_translate(cr, (job->width - image.width * sx) / 2, (job->height - image.height * sy) / 2);
	cairo_scale(cr, sx, sy);
	cairo_set_source_surface(cr, in, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	// its edges stay its edges, not smeared into the color
	cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
	cairo_rectangle(cr, 0, 0, image.width, image.height);
	cairo_fill(cr);
	cairo_destroy(cr);
	cairo_surface_destroy(in);
	image_finish(&image);
	cairo_surface_flush(out);
	job->made = out;
}

static void *worker(void *data) {
	struct flame_wallpaper *wallpaper = data;
	pthread_mutex_lock(&wallpaper->lock);
	while (!wallpaper->stop) {
		if (wl_list_empty(&wallpaper->jobs)) {
			pthread_cond_wait(&wallpaper->wake, &wallpaper->lock);
			continue;
		}
		struct wallpaper_job *job = wl_container_of(wallpaper->jobs.next, job, link);
		wl_list_remove(&job->link);
		pthread_mutex_unlock(&wallpaper->lock);
		make(job);
		pthread_mutex_lock(&wallpaper->lock);
		wl_list_insert(wallpaper->done.prev, &job->link);
		char byte = 1;
		ssize_t n = write(wallpaper->notify[1], &byte, 1);
		(void)n; // a full pipe already says it
	}
	pthread_mutex_unlock(&wallpaper->lock);
	return NULL;
}

// ---- pictures on screens ----

static void picture_drop(struct picture *picture) {
	if (picture->tree) {
		scene_node_destroy(&picture->tree->node);
	}
	if (picture->texture) {
		wlr_texture_destroy(picture->texture);
	}
	*picture = (struct picture){0};
}

static void fade_step(struct flame_anim *anim, double eased, double t) {
	struct wallpaper_screen *screen = anim->data;
	// the new one comes in over the old, or the old goes out to the color
	struct flame_tree *tree = screen->now.tree ? screen->now.tree : screen->old.tree;
	float opacity = (float)(screen->now.tree ? eased : 1 - eased);
	if (tree) {
		scene_tree_set_effect(tree, &(struct flame_effect){
			.opacity = opacity, .scale_x = 1, .scale_y = 1});
	}
}

static void fade_done(struct flame_anim *anim, bool finished) {
	struct wallpaper_screen *screen = anim->data;
	picture_drop(&screen->old);
	if (screen->now.tree) {
		// no effect: it covers what's under it again, nothing gets drawn there
		scene_tree_set_effect(screen->now.tree, NULL);
	}
}

// layout order in the background layer: the color, the wallpaper's old then
// new picture, then whatever layer surfaces are there
static void restack(struct wallpaper_screen *screen) {
	if (screen->now.tree) {
		scene_node_lower_to_bottom(&screen->now.tree->node);
	}
	if (screen->old.tree) {
		scene_node_lower_to_bottom(&screen->old.tree->node);
	}
	scene_node_lower_to_bottom(&screen->output->background->node);
}

// the mirror shows what the screen does now (no fade: it's not seen
// changing), made the first time
static void mirror_sync(struct wallpaper_screen *screen) {
	struct flame_wallpaper *wallpaper = screen->wallpaper;
	if (!wallpaper->mirror) {
		return;
	}
	if (!screen->mirror) {
		screen->mirror = scene_tree_create(wallpaper->mirror);
		screen->mirror_color = screen->mirror
			? scene_rect_create(screen->mirror, 0, 0, wallpaper->color) : NULL;
		screen->mirror_picture = screen->mirror_color
			? scene_buffer_create(screen->mirror, NULL, NULL) : NULL;
		if (!screen->mirror_picture) {
			if (screen->mirror) {
				scene_node_destroy(&screen->mirror->node);
			}
			screen->mirror = NULL;
			return;
		}
	}
	scene_node_set_position(&screen->mirror->node, screen->box.x, screen->box.y);
	scene_rect_set_size(screen->mirror_color, screen->box.width, screen->box.height);
	scene_rect_set_color(screen->mirror_color, wallpaper->color);
	scene_buffer_set(screen->mirror_picture, screen->now.texture, screen->now.texture,
		screen->box.width, screen->box.height, true, NULL, 0);
}

static void mirror_drop(struct wallpaper_screen *screen) {
	if (screen->mirror) {
		scene_node_destroy(&screen->mirror->node);
		screen->mirror = NULL;
	}
}

// now goes, faded out or not
static void fade_to(struct wallpaper_screen *screen, struct picture next) {
	struct flame_wallpaper *wallpaper = screen->wallpaper;
	anim_finish(&screen->fade); // one fading already: done with it now
	picture_drop(&screen->old);
	screen->old = screen->now;
	screen->now = next;
	restack(screen);
	mirror_sync(screen);
	const struct flame_config *config = wallpaper->server->config;
	if (!config->anim.enabled || wallpaper->server->terminating
			|| (!screen->old.tree && !screen->now.tree)) {
		picture_drop(&screen->old);
		return;
	}
	fade_step(&screen->fade, 0, 0);
	anim_start(&wallpaper->server->anims, &screen->fade, FADE_MS,
		&config->anim.fade_in_curve);
}

static void show(struct wallpaper_screen *screen, struct wallpaper_job *job) {
	struct flame_server *server = screen->wallpaper->server;
	if (!job->made) {
		wlr_log(WLR_ERROR, "wallpaper: %s: %s", job->path, job->error);
		fade_to(screen, (struct picture){0});
		return;
	}
	struct picture picture = {0};
	picture.texture = wlr_texture_from_pixels(server->render.renderer, DRM_FORMAT_XRGB8888,
		(uint32_t)cairo_image_surface_get_stride(job->made), (uint32_t)job->width,
		(uint32_t)job->height, cairo_image_surface_get_data(job->made));
	picture.tree = picture.texture
		? scene_tree_create(server->render.layers[FLAME_LAYER_BACKGROUND]) : NULL;
	picture.node = picture.tree ? scene_buffer_create(picture.tree, NULL, NULL) : NULL;
	if (!picture.node) {
		wlr_log(WLR_ERROR, "wallpaper: no texture for %s on %s", job->path,
			screen->output->wlr->name);
		picture_drop(&picture);
		return;
	}
	scene_node_set_position(&picture.tree->node, screen->box.x, screen->box.y);
	scene_buffer_set(picture.node, picture.texture, picture.texture, screen->box.width,
		screen->box.height, true, NULL, 0);
	fade_to(screen, picture);
}

static int handle_notify(int fd, uint32_t mask, void *data) {
	struct flame_wallpaper *wallpaper = data;
	char drain[64];
	while (read(fd, drain, sizeof(drain)) > 0) {
	}
	struct wl_list done;
	wl_list_init(&done);
	pthread_mutex_lock(&wallpaper->lock);
	wl_list_insert_list(&done, &wallpaper->done);
	wl_list_init(&wallpaper->done);
	pthread_mutex_unlock(&wallpaper->lock);

	struct wallpaper_job *job, *tmp;
	wl_list_for_each_safe(job, tmp, &done, link) {
		wl_list_remove(&job->link);
		// still the one its screen waits for: show it. a newer one's on
		// its way, or the screen went
		struct wallpaper_screen *screen;
		wl_list_for_each(screen, &wallpaper->screens, link) {
			if (screen->id == job->screen && screen->waiting == job->id) {
				screen->waiting = 0;
				show(screen, job);
				break;
			}
		}
		job_free(job);
	}
	return 0;
}

// screen's picture made again for what it is now
static void remake(struct wallpaper_screen *screen) {
	struct flame_wallpaper *wallpaper = screen->wallpaper;
	pthread_mutex_lock(&wallpaper->lock);
	// one still queued for it is of no use now
	struct wallpaper_job *job, *tmp;
	wl_list_for_each_safe(job, tmp, &wallpaper->jobs, link) {
		if (job->screen == screen->id) {
			wl_list_remove(&job->link);
			job_free(job);
		}
	}
	pthread_mutex_unlock(&wallpaper->lock);
	screen->waiting = 0;
	if (!wallpaper->path || !screen->width || !wallpaper->thread_up) {
		fade_to(screen, (struct picture){0});
		return;
	}
	job = calloc(1, sizeof(*job));
	if (!job || !(job->path = strdup(wallpaper->path))) {
		free(job);
		return;
	}
	job->id = ++wallpaper->next_id;
	job->screen = screen->id;
	job->mode = wallpaper->mode;
	memcpy(job->color, wallpaper->color, sizeof(job->color));
	job->width = screen->width;
	job->height = screen->height;
	screen->waiting = job->id;
	pthread_mutex_lock(&wallpaper->lock);
	wl_list_insert(wallpaper->jobs.prev, &job->link);
	pthread_cond_signal(&wallpaper->wake);
	pthread_mutex_unlock(&wallpaper->lock);
}

static struct wallpaper_screen *screen_of(struct flame_wallpaper *wallpaper,
		struct flame_output *output) {
	struct wallpaper_screen *screen;
	wl_list_for_each(screen, &wallpaper->screens, link) {
		if (screen->output == output) {
			return screen;
		}
	}
	return NULL;
}

void wallpaper_output_changed(struct flame_wallpaper *wallpaper, struct flame_output *output) {
	struct wallpaper_screen *screen = screen_of(wallpaper, output);
	if (!screen) {
		screen = calloc(1, sizeof(*screen));
		if (!screen) {
			return;
		}
		screen->wallpaper = wallpaper;
		screen->output = output;
		screen->id = ++wallpaper->next_id;
		screen->fade = (struct flame_anim){.step = fade_step, .done = fade_done, .data = screen};
		wl_list_insert(wallpaper->screens.prev, &screen->link);
	}
	screen->box = output->box;
	struct picture *pictures[] = {&screen->now, &screen->old};
	for (int i = 0; i < 2; i++) {
		if (pictures[i]->tree) {
			scene_node_set_position(&pictures[i]->tree->node, screen->box.x, screen->box.y);
			scene_buffer_set(pictures[i]->node, pictures[i]->texture, pictures[i]->texture,
				screen->box.width, screen->box.height, true, NULL, 0);
		}
	}
	mirror_sync(screen);
	int width, height;
	wlr_output_transformed_resolution(output->wlr, &width, &height);
	if (width != screen->width || height != screen->height) {
		screen->width = width;
		screen->height = height;
		remake(screen);
	}
}

void wallpaper_output_gone(struct flame_wallpaper *wallpaper, struct flame_output *output) {
	struct wallpaper_screen *screen = wallpaper->screens.next ? screen_of(wallpaper, output)
		: NULL;
	if (!screen) {
		return;
	}
	anim_cancel(&screen->fade);
	mirror_drop(screen); // before the picture it shows goes
	picture_drop(&screen->now);
	picture_drop(&screen->old);
	wl_list_remove(&screen->link);
	free(screen); // a job of its still out finds no screen by its id
}

void wallpaper_config_changed(struct flame_wallpaper *wallpaper) {
	const struct flame_config *config = wallpaper->server->config;
	const char *path = wallpaper->instead ? wallpaper->instead : config->wallpaper;
	bool same = (!wallpaper->path == !path)
		&& (!wallpaper->path || !strcmp(wallpaper->path, path))
		&& wallpaper->mode == config->wallpaper_mode
		&& !memcmp(wallpaper->color, config->background_color, sizeof(wallpaper->color));
	if (same) {
		return;
	}
	free(wallpaper->path);
	wallpaper->path = path ? strdup(path) : NULL;
	wallpaper->mode = config->wallpaper_mode;
	memcpy(wallpaper->color, config->background_color, sizeof(wallpaper->color));
	struct wallpaper_screen *screen;
	wl_list_for_each(screen, &wallpaper->screens, link) {
		mirror_sync(screen); // the color now, the picture once it's made
		remake(screen);
	}
}

void wallpaper_instead(struct flame_wallpaper *wallpaper, const char *path) {
	free(wallpaper->instead);
	wallpaper->instead = path ? strdup(path) : NULL;
	wallpaper_config_changed(wallpaper);
}

bool wallpaper_init(struct flame_wallpaper *wallpaper, struct flame_server *server) {
	*wallpaper = (struct flame_wallpaper){.server = server, .notify = {-1, -1}};
	wl_list_init(&wallpaper->screens);
	wl_list_init(&wallpaper->jobs);
	wl_list_init(&wallpaper->done);
	pthread_mutex_init(&wallpaper->lock, NULL);
	pthread_cond_init(&wallpaper->wake, NULL);
	const struct flame_config *config = server->config;
	wallpaper->path = config->wallpaper ? strdup(config->wallpaper) : NULL;
	wallpaper->mode = config->wallpaper_mode;
	memcpy(wallpaper->color, config->background_color, sizeof(wallpaper->color));
	wallpaper->mirror = server->lock.backdrop;
	// without the worker flame still runs, just without a wallpaper
	bool piped = pipe(wallpaper->notify) == 0;
	for (int i = 0; piped && i < 2; i++) {
		piped = fcntl(wallpaper->notify[i], F_SETFD, FD_CLOEXEC) == 0
			&& fcntl(wallpaper->notify[i], F_SETFL, O_NONBLOCK) == 0;
	}
	if (!piped) {
		wlr_log(WLR_ERROR, "wallpaper: no pipe, no wallpaper");
		return true;
	}
	wallpaper->notify_source = wl_event_loop_add_fd(server->loop, wallpaper->notify[0],
		WL_EVENT_READABLE, handle_notify, wallpaper);
	wallpaper->thread_up = wallpaper->notify_source
		&& !pthread_create(&wallpaper->thread, NULL, worker, wallpaper);
	if (!wallpaper->thread_up) {
		wlr_log(WLR_ERROR, "wallpaper: no worker thread, no wallpaper");
	}
	return true;
}

void wallpaper_finish(struct flame_wallpaper *wallpaper) {
	if (wallpaper->thread_up) {
		pthread_mutex_lock(&wallpaper->lock);
		wallpaper->stop = true;
		pthread_cond_signal(&wallpaper->wake);
		pthread_mutex_unlock(&wallpaper->lock);
		pthread_join(wallpaper->thread, NULL);
	}
	struct wallpaper_screen *screen, *tmp_screen;
	wl_list_for_each_safe(screen, tmp_screen, &wallpaper->screens, link) {
		wallpaper_output_gone(wallpaper, screen->output);
	}
	struct wl_list *lists[] = {&wallpaper->jobs, &wallpaper->done};
	for (int i = 0; i < 2; i++) {
		struct wallpaper_job *job, *tmp;
		wl_list_for_each_safe(job, tmp, lists[i], link) {
			wl_list_remove(&job->link);
			job_free(job);
		}
	}
	if (wallpaper->notify_source) {
		wl_event_source_remove(wallpaper->notify_source);
	}
	for (int i = 0; i < 2; i++) {
		if (wallpaper->notify[i] >= 0) {
			close(wallpaper->notify[i]);
		}
	}
	free(wallpaper->instead);
	wallpaper->instead = NULL;
	pthread_mutex_destroy(&wallpaper->lock);
	pthread_cond_destroy(&wallpaper->wake);
	free(wallpaper->path);
	wallpaper->path = NULL;
}
