#ifndef FLAME_CURSOR_IMAGE_H
#define FLAME_CURSOR_IMAGE_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct wlr_surface;
struct wlr_xcursor_image;
struct wlr_xcursor_manager;

// what the pointer looks like: a theme cursor by name (animated ones keep
// going), a client's own cursor surface, an X client's pixels, or nothing. flame owns this now,
// wlroots never gets an image, so its cursor code never draws or blocks
// direct scanout. outputs pick it up per frame and show it on the cursor
// plane or draw it themselves

enum flame_cursor_kind {
	FLAME_CURSOR_HIDDEN,
	FLAME_CURSOR_NAMED,
	FLAME_CURSOR_SURFACE,
	FLAME_CURSOR_PIXELS,
};

// a frame of a cursor in pixels
struct flame_cursor_pixels {
	const uint32_t *pixels; // ARGB premultiplied, width a row
	int width, height, hotspot_x, hotspot_y;
	uint32_t delay; // ms on it, animated ones
};

struct flame_cursor_image {
	struct wl_event_loop *loop;
	struct wlr_xcursor_manager *theme;
	enum flame_cursor_kind kind;
	// bumped whenever it's set to something, a new surface can reuse a
	// freed one's address
	uint32_t generation;

	char name[64];
	int64_t started; // ms, animations count from here
	struct wl_event_source *animate;

	struct wlr_surface *surface;
	int hotspot_x, hotspot_y; // surface local
	struct wl_listener surface_commit;
	struct wl_listener surface_destroy;

	// pixels: frames like a theme cursor's, at scale (pixels a layout unit)
	struct wlr_xcursor *pixels;
	float pixels_scale;
	uint64_t pixels_key;

	struct {
		struct wl_signal changed;
	} events;
};

// what to show on an output with this scale, right now
struct flame_cursor_frame {
	enum flame_cursor_kind kind;
	// named, pixels: the image, hotspot in its pixels, scale its pixels a
	// layout unit (a theme loads at the output's)
	const struct wlr_xcursor_image *xcursor;
	float scale;
	// surface: the client's cursor, hotspot in surface coords
	struct wlr_surface *surface;
	int hotspot_x, hotspot_y;
};

bool cursor_image_init(struct flame_cursor_image *image, struct wl_event_loop *loop,
	const char *theme, uint32_t size);
void cursor_image_finish(struct flame_cursor_image *image);

void cursor_image_set_name(struct flame_cursor_image *image, const char *name);
// NULL hides it, like wl_pointer.set_cursor
void cursor_image_set_surface(struct flame_cursor_image *image, struct wlr_surface *surface,
	int hotspot_x, int hotspot_y);

// frames of pixels at scale. the same key again is the same look: nothing
// changes, an animation keeps its phase
void cursor_image_set_pixels(struct flame_cursor_image *image, uint64_t key,
	const struct flame_cursor_pixels *frames, size_t len, float scale);

void cursor_image_get(struct flame_cursor_image *image, float scale,
	struct flame_cursor_frame *frame);

#endif
