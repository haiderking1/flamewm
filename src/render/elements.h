#ifndef FLAME_RENDER_ELEMENTS_H
#define FLAME_RENDER_ELEMENTS_H

#include <pixman.h>
#include <stdbool.h>
#include <stddef.h>
#include <wayland-server-protocol.h>
#include <wlr/util/box.h>

#include "scene/scene.h"

struct flame_buffer_node;
struct flame_scene;
struct wlr_drm_syncobj_timeline;
struct wlr_buffer;
struct wlr_surface;
struct wlr_texture;

// one thing a frame draws, flattened out of the scene: a solid rect, one
// client surface (subsurfaces are elements of their own) or a texture of
// ours (the pointer). bottom to top
enum flame_element_type {
	FLAME_ELEMENT_RECT,
	FLAME_ELEMENT_SURFACE,
	FLAME_ELEMENT_TEXTURE,
	// a surface destroyed after the list was built: only its id and boxes
	// are left, for the damage diff. nothing to draw or call back
	FLAME_ELEMENT_GONE,
	// frosted glass over what's below it: drawn from what's already in the
	// buffer, so wherever it's drawn everything under its reach has to be
	FLAME_ELEMENT_GLASS,
};

struct flame_element {
	enum flame_element_type type;
	// what it is from one frame to the next: the rect node or the wl_surface.
	// only ever compared, an id from an old frame may point at freed memory
	const void *id;
	struct wlr_box box; // layout coords, where it's drawn
	struct wlr_box clip; // layout coords, the part of box allowed to show

	float color[4]; // rects, premultiplied
	// glass, lengths in layout units scaled like the box
	struct flame_glass_style glass;
	// glass of an app's: its surface. a surface's panes frost what's behind
	// the surface, not each other (see pass_glass_group). NULL for flame's own
	const void *glass_group;

	struct wlr_surface *surface; // surfaces
	// the rounded window a surface is part of (layout coords, like box):
	// outside it nothing of it shows. a radius of 0 for none
	struct wlr_box shape;
	float shape_radius;
	struct wlr_texture *texture;
	struct wlr_fbox src;
	enum wl_output_transform transform;
	struct wlr_buffer *buffer; // to tell a new buffer from the same one
	// textures: what's on it. ours get recreated, maybe at the same address
	const void *image;
	bool opaque_texture;
	// textures from buffer nodes: whose, and the gpu work to wait for
	struct flame_buffer_node *buffer_node;
	struct wlr_drm_syncobj_timeline *wait_timeline;
	uint64_t wait_point;

	// animations: drawn this see through (1 for not at all), and whether it's
	// scaled or faded at all: then it covers nothing and never goes to scanout.
	// only moved isn't effected
	float alpha;
	bool effected;
	// a surface showing nothing (one pixel of no colour, a single-pixel
	// buffer): never drawn, never in the way of scanout, still a surface
	// that's on screen (its frames, enter and leave)
	bool clear;
	// from a tree under every window (see flame_tree.under_windows)
	bool under_windows;
	// glass: reads the kept blur of what's under the windows instead of
	// what's drawn (nothing else is behind it). set by whoever draws it
	bool from_kept;

	// layout coords: what isn't covered by anything opaque above it, and
	// what of it is opaque itself. filled by elements_occlude
	pixman_region32_t visible;
	pixman_region32_t opaque;
};

struct flame_element_list {
	struct flame_element *data;
	size_t len, cap;
};

void elements_init(struct flame_element_list *list);
void elements_finish(struct flame_element_list *list);
void elements_clear(struct flame_element_list *list);
// surface is being destroyed: its elements turn into FLAME_ELEMENT_GONE
void elements_forget_surface(struct flame_element_list *list, struct wlr_surface *surface);

// everything enabled in the scene that lands on area, bottom to top
bool elements_collect(struct flame_element_list *list, struct flame_scene *scene,
	struct wlr_box area);
// the same for one node and what's under it, as if the node's parent sat at
// 0, 0 (effects above it don't count): a window's snapshot
struct flame_node;
bool elements_collect_node(struct flame_element_list *list, struct flame_node *node,
	struct wlr_box area);
// a surface and its subsurfaces with the surface at x, y, on top of the list
bool elements_push_surface(struct flame_element_list *list, struct wlr_surface *surface,
	int x, int y, struct wlr_box area);
// a translucent texture drawn into box, on top of the list
bool elements_push_texture(struct flame_element_list *list, const void *id,
	struct wlr_texture *texture, const void *image, struct wlr_box box, struct wlr_box area);

// what list looks like drawn into area at scale, as a number: a different
// one when anything in it moved, showed up, changed buffer or got a commit
// into the same buffer. window capture and previews redraw only then
uint64_t elements_look(const struct flame_element_list *list, struct wlr_box area,
	float scale);

// fills visible and opaque, top down
void elements_occlude(struct flame_element_list *list);

// how far around its box glass reads what's behind it (blur and
// refraction), layout units
int elements_glass_reach(const struct flame_element *element);

// the same element in another frame's list, NULL when it wasn't there. hint
// is where to look first, lists barely change between frames
const struct flame_element *elements_find(const struct flame_element_list *list,
	const void *id, size_t hint, size_t *index);

#endif
