#define _GNU_SOURCE // memfd_create
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "content-type-v1-client-protocol.h"
#include "internal.h"
#include "viewporter-client-protocol.h"

// a layer: a surface of its own under the window's, showing pictures the
// app hands it (a video's frames). flame scales them (the viewporter), they
// change on their own (a desynchronized subsurface) without the window
// being drawn again, and the window's glass over them frosts them

struct fui_picture {
	struct fui_layer *layer;
	struct wl_buffer *buffer;
	void *pixels;
	size_t size;
	int width, height, stride;
	bool on_screen; // attached and not let go of yet
};

struct fui_layer {
	struct fui *ui;
	struct wl_surface *surface;
	struct wl_subsurface *subsurface;
	struct wp_viewport *viewport;
	struct wp_content_type_v1 *content_type;
	struct wl_callback *frame;
	void (*frame_f)(void *data);
	void *frame_data;
	void (*released)(struct fui_picture *picture, void *data);
	void *released_data;
	struct fui_picture *shown;
	int x, y, w, h; // where it is, window coords
};

struct fui_layer *fui_layer_create(struct fui *ui) {
	if (!ui->subcompositor || !ui->viewporter) {
		return NULL;
	}
	struct fui_layer *layer = calloc(1, sizeof(*layer));
	if (!layer) {
		return NULL;
	}
	layer->ui = ui;
	layer->surface = wl_compositor_create_surface(ui->compositor);
	layer->subsurface = wl_subcompositor_get_subsurface(ui->subcompositor, layer->surface,
		ui->surface);
	wl_subsurface_place_below(layer->subsurface, ui->surface);
	wl_subsurface_set_desync(layer->subsurface);
	layer->viewport = wp_viewporter_get_viewport(ui->viewporter, layer->surface);
	// it's a video: flame takes a frame of it scaled to a new size as done
	// (a paused one doesn't hold a resize up), and may treat it as one
	if (ui->content_type) {
		layer->content_type = wp_content_type_manager_v1_get_surface_content_type(
			ui->content_type, layer->surface);
		wp_content_type_v1_set_content_type(layer->content_type,
			WP_CONTENT_TYPE_V1_TYPE_VIDEO);
	}
	// takes no input: the window over it does
	struct wl_region *none = wl_compositor_create_region(ui->compositor);
	wl_surface_set_input_region(layer->surface, none);
	wl_region_destroy(none);
	return layer;
}

void fui_layer_destroy(struct fui_layer *layer) {
	if (!layer) {
		return;
	}
	if (layer->frame) {
		wl_callback_destroy(layer->frame);
	}
	if (layer->content_type) {
		wp_content_type_v1_destroy(layer->content_type);
	}
	wp_viewport_destroy(layer->viewport);
	wl_subsurface_destroy(layer->subsurface);
	wl_surface_destroy(layer->surface);
	free(layer);
}

static void picture_release(void *data, struct wl_buffer *buffer) {
	struct fui_picture *picture = data;
	struct fui_layer *layer = picture->layer;
	picture->on_screen = false;
	if (layer->released) {
		layer->released(picture, layer->released_data);
	}
}

static const struct wl_buffer_listener picture_listener = {picture_release};

struct fui_picture *fui_picture_create(struct fui_layer *layer, int width, int height) {
	struct fui_picture *picture = calloc(1, sizeof(*picture));
	if (!picture) {
		return NULL;
	}
	picture->layer = layer;
	picture->width = width;
	picture->height = height;
	picture->stride = width * 4;
	picture->size = (size_t)picture->stride * (size_t)height;
	int fd = memfd_create("flameui-picture", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t)picture->size) < 0) {
		if (fd >= 0) {
			close(fd);
		}
		free(picture);
		return NULL;
	}
	picture->pixels = mmap(NULL, picture->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (picture->pixels == MAP_FAILED) {
		close(fd);
		free(picture);
		return NULL;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(layer->ui->shm, fd, (int32_t)picture->size);
	// a video's frame has no alpha: flame needn't blend it
	picture->buffer = wl_shm_pool_create_buffer(pool, 0, width, height, picture->stride,
		WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	wl_buffer_add_listener(picture->buffer, &picture_listener, picture);
	return picture;
}

void fui_picture_destroy(struct fui_picture *picture) {
	if (!picture) {
		return;
	}
	if (picture->layer->shown == picture) {
		picture->layer->shown = NULL;
	}
	wl_buffer_destroy(picture->buffer);
	munmap(picture->pixels, picture->size);
	free(picture);
}

void *fui_picture_pixels(struct fui_picture *picture, int *stride) {
	*stride = picture->stride;
	return picture->pixels;
}

bool fui_picture_on_screen(const struct fui_picture *picture) {
	return picture->on_screen;
}

static void place(struct fui_layer *layer, double x, double y, double w, double h) {
	int ix = (int)(x + 0.5), iy = (int)(y + 0.5), iw = (int)(w + 0.5), ih = (int)(h + 0.5);
	iw = iw < 1 ? 1 : iw;
	ih = ih < 1 ? 1 : ih;
	if (ix != layer->x || iy != layer->y) {
		// a subsurface's position goes with its parent's next commit
		wl_subsurface_set_position(layer->subsurface, ix, iy);
		fui_redraw(layer->ui);
	}
	layer->x = ix;
	layer->y = iy;
	layer->w = iw;
	layer->h = ih;
	wp_viewport_set_destination(layer->viewport, iw, ih);
}

void fui_layer_show(struct fui_layer *layer, struct fui_picture *picture, double x, double y,
		double w, double h) {
	place(layer, x, y, w, h);
	if (picture) {
		wl_surface_attach(layer->surface, picture->buffer, 0, 0);
		wl_surface_damage_buffer(layer->surface, 0, 0, picture->width, picture->height);
		picture->on_screen = true;
		layer->shown = picture;
	}
	wl_surface_commit(layer->surface);
}

void fui_layer_place(struct fui_layer *layer, double x, double y, double w, double h) {
	fui_layer_show(layer, NULL, x, y, w, h);
}

void fui_layer_on_release(struct fui_layer *layer,
		void (*released)(struct fui_picture *picture, void *data), void *data) {
	layer->released = released;
	layer->released_data = data;
}

static void layer_frame_done(void *data, struct wl_callback *callback, uint32_t time) {
	struct fui_layer *layer = data;
	wl_callback_destroy(callback);
	layer->frame = NULL;
	if (layer->frame_f) {
		layer->frame_f(layer->frame_data);
	}
}

static const struct wl_callback_listener layer_frame_listener = {layer_frame_done};

void fui_layer_frame(struct fui_layer *layer, void (*f)(void *data), void *data) {
	layer->frame_f = f;
	layer->frame_data = data;
	if (layer->frame) {
		return; // one asked for already: f's the one it calls
	}
	layer->frame = wl_surface_frame(layer->surface);
	wl_callback_add_listener(layer->frame, &layer_frame_listener, layer);
	wl_surface_commit(layer->surface);
}
