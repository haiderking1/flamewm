#include "cursor/image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/util/log.h>
#include <wlr/xcursor.h>

#include "util/listener.h"

static int64_t now_ms(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void changed(struct flame_cursor_image *image) {
	wl_signal_emit_mutable(&image->events.changed, image);
}

// the theme's cursor for the current name at this scale
static struct wlr_xcursor *named(struct flame_cursor_image *image, float scale) {
	if (!wlr_xcursor_manager_load(image->theme, scale)) {
		return NULL;
	}
	struct wlr_xcursor *xcursor = wlr_xcursor_manager_get_xcursor(image->theme,
		image->name, scale);
	if (!xcursor && strcmp(image->name, "default")) {
		// a shape the theme doesn't have still gets a pointer
		xcursor = wlr_xcursor_manager_get_xcursor(image->theme, "default", scale);
	}
	return xcursor;
}

static void drop_pixels(struct flame_cursor_image *image) {
	struct wlr_xcursor *xcursor = image->pixels;
	if (!xcursor) {
		return;
	}
	for (unsigned i = 0; i < xcursor->image_count; i++) {
		free(xcursor->images[i]->buffer);
		free(xcursor->images[i]);
	}
	free(xcursor->images);
	free(xcursor);
	image->pixels = NULL;
	image->pixels_key = 0;
}

// what shows, theme or pixels
static struct wlr_xcursor *current(struct flame_cursor_image *image, float scale) {
	if (image->kind == FLAME_CURSOR_PIXELS) {
		return image->pixels;
	}
	return image->kind == FLAME_CURSOR_NAMED ? named(image, scale) : NULL;
}

// the next frame of an animated cursor, a single frame needs no timer
static void schedule_animation(struct flame_cursor_image *image) {
	wl_event_source_timer_update(image->animate, 0);
	// every scale shares the theme's frame delays, scale 1 is always loaded
	struct wlr_xcursor *xcursor = current(image, 1);
	if (!xcursor || xcursor->image_count < 2 || !xcursor->total_delay) {
		return;
	}
	uint32_t elapsed = (uint32_t)((now_ms() - image->started) % xcursor->total_delay);
	uint32_t edge = 0;
	for (unsigned i = 0; i < xcursor->image_count; i++) {
		// a zero delay still moves on
		edge += xcursor->images[i]->delay ? xcursor->images[i]->delay : 1;
		if (edge > elapsed) {
			wl_event_source_timer_update(image->animate, (int)(edge - elapsed));
			return;
		}
	}
	wl_event_source_timer_update(image->animate, 1);
}

static int handle_animate(void *data) {
	struct flame_cursor_image *image = data;
	changed(image);
	schedule_animation(image);
	return 0;
}

static void drop_surface(struct flame_cursor_image *image) {
	if (image->surface) {
		listener_disconnect(&image->surface_commit);
		listener_disconnect(&image->surface_destroy);
		image->surface = NULL;
	}
}

static void handle_surface_commit(struct wl_listener *listener, void *data) {
	struct flame_cursor_image *image = wl_container_of(listener, image, surface_commit);
	// attaching with an offset moves the hotspot the other way
	image->hotspot_x -= image->surface->current.dx;
	image->hotspot_y -= image->surface->current.dy;
	changed(image);
}

static void handle_surface_destroy(struct wl_listener *listener, void *data) {
	struct flame_cursor_image *image = wl_container_of(listener, image, surface_destroy);
	drop_surface(image);
	image->kind = FLAME_CURSOR_HIDDEN;
	image->generation++;
	changed(image);
}

bool cursor_image_init(struct flame_cursor_image *image, struct wl_event_loop *loop,
		const char *theme, uint32_t size) {
	memset(image, 0, sizeof(*image));
	image->loop = loop;
	wl_signal_init(&image->events.changed);
	wl_list_init(&image->surface_commit.link);
	wl_list_init(&image->surface_destroy.link);
	image->theme = wlr_xcursor_manager_create(theme, size);
	image->animate = wl_event_loop_add_timer(loop, handle_animate, image);
	if (!image->theme || !image->animate || !wlr_xcursor_manager_load(image->theme, 1)) {
		wlr_log(WLR_ERROR, "failed to load the cursor theme");
		cursor_image_finish(image);
		return false;
	}
	cursor_image_set_name(image, "default");
	return true;
}

void cursor_image_finish(struct flame_cursor_image *image) {
	drop_surface(image);
	drop_pixels(image);
	if (image->animate) {
		wl_event_source_remove(image->animate);
		image->animate = NULL;
	}
	if (image->theme) {
		wlr_xcursor_manager_destroy(image->theme);
		image->theme = NULL;
	}
}

void cursor_image_set_name(struct flame_cursor_image *image, const char *name) {
	if (image->kind == FLAME_CURSOR_NAMED && !strcmp(image->name, name)) {
		return; // same shape, the animation keeps its phase
	}
	drop_surface(image);
	drop_pixels(image);
	image->kind = FLAME_CURSOR_NAMED;
	image->generation++;
	snprintf(image->name, sizeof(image->name), "%s", name);
	image->started = now_ms();
	schedule_animation(image);
	changed(image);
}

void cursor_image_set_surface(struct flame_cursor_image *image, struct wlr_surface *surface,
		int hotspot_x, int hotspot_y) {
	if (surface == image->surface && image->kind == FLAME_CURSOR_SURFACE
			&& hotspot_x == image->hotspot_x && hotspot_y == image->hotspot_y) {
		return;
	}
	drop_surface(image);
	drop_pixels(image);
	wl_event_source_timer_update(image->animate, 0);
	image->kind = surface ? FLAME_CURSOR_SURFACE : FLAME_CURSOR_HIDDEN;
	image->generation++;
	image->hotspot_x = hotspot_x;
	image->hotspot_y = hotspot_y;
	if (surface) {
		image->surface = surface;
		listener_connect(&image->surface_commit, &surface->events.commit,
			handle_surface_commit);
		listener_connect(&image->surface_destroy, &surface->events.destroy,
			handle_surface_destroy);
	}
	changed(image);
}

// a copy of frames as a theme cursor looks, NULL without the memory
static struct wlr_xcursor *pixels_copy(const struct flame_cursor_pixels *frames, size_t len) {
	struct wlr_xcursor *xcursor = calloc(1, sizeof(*xcursor));
	if (!xcursor || !(xcursor->images = calloc(len, sizeof(*xcursor->images)))) {
		free(xcursor);
		return NULL;
	}
	for (size_t i = 0; i < len; i++) {
		const struct flame_cursor_pixels *from = &frames[i];
		size_t bytes = (size_t)from->width * (size_t)from->height * 4;
		struct wlr_xcursor_image *to = calloc(1, sizeof(*to));
		uint8_t *buffer = to ? malloc(bytes) : NULL;
		if (!buffer) {
			free(to);
			break;
		}
		memcpy(buffer, from->pixels, bytes);
		*to = (struct wlr_xcursor_image){
			.width = (uint32_t)from->width,
			.height = (uint32_t)from->height,
			.hotspot_x = (uint32_t)from->hotspot_x,
			.hotspot_y = (uint32_t)from->hotspot_y,
			.delay = len > 1 ? from->delay : 0,
			.buffer = buffer,
		};
		xcursor->images[xcursor->image_count++] = to;
		xcursor->total_delay += to->delay;
	}
	return xcursor;
}

void cursor_image_set_pixels(struct flame_cursor_image *image, uint64_t key,
		const struct flame_cursor_pixels *frames, size_t len, float scale) {
	if (image->kind == FLAME_CURSOR_PIXELS && image->pixels_key == key
			&& image->pixels_scale == scale) {
		return;
	}
	drop_surface(image);
	drop_pixels(image);
	image->pixels = len ? pixels_copy(frames, len) : NULL;
	if (image->pixels && image->pixels->image_count != len) {
		drop_pixels(image); // half of it: none
	}
	image->kind = image->pixels ? FLAME_CURSOR_PIXELS : FLAME_CURSOR_HIDDEN;
	image->pixels_key = key;
	image->pixels_scale = scale;
	image->generation++;
	image->started = now_ms();
	schedule_animation(image);
	changed(image);
}

void cursor_image_get(struct flame_cursor_image *image, float scale,
		struct flame_cursor_frame *frame) {
	*frame = (struct flame_cursor_frame){.kind = FLAME_CURSOR_HIDDEN};
	if (image->kind == FLAME_CURSOR_SURFACE) {
		if (!image->surface->mapped) {
			return; // nothing attached yet
		}
		frame->kind = FLAME_CURSOR_SURFACE;
		frame->surface = image->surface;
		frame->hotspot_x = image->hotspot_x;
		frame->hotspot_y = image->hotspot_y;
		return;
	}
	struct wlr_xcursor *xcursor = current(image, scale);
	if (!xcursor || !xcursor->image_count) {
		return;
	}
	int index = xcursor->image_count > 1
		? wlr_xcursor_frame(xcursor, (uint32_t)(now_ms() - image->started)) : 0;
	frame->kind = image->kind;
	frame->scale = image->kind == FLAME_CURSOR_PIXELS ? image->pixels_scale : scale;
	frame->xcursor = xcursor->images[index];
	frame->hotspot_x = (int)frame->xcursor->hotspot_x;
	frame->hotspot_y = (int)frame->xcursor->hotspot_y;
}
