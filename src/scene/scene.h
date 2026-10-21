#ifndef FLAME_SCENE_SCENE_H
#define FLAME_SCENE_SCENE_H

#include <pixman.h>
#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wlr/util/box.h>

struct flame_buffer_node;
struct flame_frame;
struct wlr_drm_syncobj_timeline;
struct wlr_surface;
struct wlr_texture;

// flame's own scene graph: what goes where, in which order. it draws nothing.
// render/ walks it every frame and works out damage by comparing against what
// it drew last time, so changing a node is cheap, it only asks for a frame

enum flame_node_type {
	FLAME_NODE_TREE,
	FLAME_NODE_RECT,
	FLAME_NODE_SURFACE,
	FLAME_NODE_BUFFER,
};

struct flame_scene;
struct flame_tree;

struct flame_node {
	enum flame_node_type type;
	struct flame_scene *scene;
	struct flame_tree *parent; // NULL for the root
	struct wl_list link; // parent->children, bottom to top
	bool enabled;
	// drawn but not there for the pointer: a workspace sliding away
	bool passthrough;
	int x, y; // relative to the parent
	void *data; // views point this back at themselves

	struct {
		struct wl_signal destroy;
	} events;
};

// how a tree and everything in it is drawn, for animations: scaled about
// origin (tree coords), moved by dx, dy, faded to opacity. hit testing and
// positions don't see it, only what gets drawn
struct flame_effect {
	float opacity;
	double scale_x, scale_y;
	double origin_x, origin_y;
	double dx, dy;
};

struct flame_tree {
	struct flame_node node;
	struct wl_list children; // flame_node.link
	bool has_effect;
	struct flame_effect effect;
};

struct flame_rect {
	struct flame_node node;
	int width, height;
	float color[4]; // premultiplied
};

// a client surface and its subsurfaces, drawn in protocol order
struct flame_surface_node {
	struct flame_node node;
	struct wlr_surface *surface; // NULL once the client destroyed it
	struct wlr_box clip; // root surface local, empty for none
	struct wl_listener surface_destroy;
};

// what the owner of a buffer node hears about it
struct flame_buffer_node_impl {
	// a frame drew it: that frame goes out as draw_id on frame's output (see
	// flame_frame events.presented), and the gpu is done reading the buffer
	// once render_done reaches render_point. NULL timeline: done already
	void (*sampled)(struct flame_buffer_node *node, struct flame_frame *frame,
		uint64_t draw_id, struct wlr_drm_syncobj_timeline *render_done,
		uint64_t render_point);
	// a point (node coords) that shows but doesn't take input, a shadow:
	// what's below gets it. NULL: everything does
	bool (*accepts_input)(struct flame_buffer_node *node, double x, double y);
};

// a picture flame shows for something that isn't a wl_surface (X11 windows):
// the owner hands it a texture and says when it changes
struct flame_buffer_node {
	struct flame_node node;
	struct wlr_texture *texture; // the owner's, NULL draws nothing
	// a new value for every new picture, the frame repaints it all then
	const void *key;
	int width, height; // layout size it's drawn at
	struct wlr_fbox src; // the part of texture shown, empty for all of it
	bool opaque;
	// takes the pointer over its whole size even with nothing to show: an
	// X window gets input by where it is, drawn yet or not
	bool input_when_empty;
	// the gpu work that made the picture, waited on before reading it
	struct wlr_drm_syncobj_timeline *wait_timeline;
	uint64_t wait_point;
	const struct flame_buffer_node_impl *impl;
	void *data;
};

struct flame_scene {
	struct flame_tree root;
	// bumped every time a frame gets built from the scene, surfaces remember
	// the last one they were in
	uint64_t frame_seq;
	// app frames that came in after the render they were woken just in time
	// for, frame timing counts them
	uint32_t late_app_frames;

	struct {
		// something moved, showed up, went away or looks different
		struct wl_signal changed;
		// a client redrew part of itself, pixman_region32_t in layout coords
		struct wl_signal damage;
		// any surface in the scene committed, on screen or not (struct
		// wlr_surface): window capture follows its window by this
		struct wl_signal commit;
		// a wl_surface is going away (struct wlr_surface), whatever holds on
		// to it lets go
		struct wl_signal surface_destroy;
	} events;
};

void scene_init(struct flame_scene *scene);
void scene_finish(struct flame_scene *scene);
// something about the scene changed that isn't a node, a surface committed
void scene_changed(struct flame_scene *scene);

struct flame_tree *scene_tree_create(struct flame_tree *parent);
// NULL takes it off: drawn as it is
void scene_tree_set_effect(struct flame_tree *tree, const struct flame_effect *effect);
struct flame_rect *scene_rect_create(struct flame_tree *parent, int width, int height,
	const float color[4]);
struct flame_surface_node *scene_surface_create(struct flame_tree *parent,
	struct wlr_surface *surface);
struct flame_buffer_node *scene_buffer_create(struct flame_tree *parent,
	const struct flame_buffer_node_impl *impl, void *data);

// trees take their children with them
void scene_node_destroy(struct flame_node *node);
void scene_node_set_enabled(struct flame_node *node, bool enabled);
void scene_node_set_position(struct flame_node *node, int x, int y);
void scene_node_reparent(struct flame_node *node, struct flame_tree *parent);
void scene_node_raise_to_top(struct flame_node *node);
void scene_node_lower_to_bottom(struct flame_node *node);
// layout coords, false when it or anything above it is disabled
bool scene_node_coords(struct flame_node *node, int *lx, int *ly);

void scene_rect_set_size(struct flame_rect *rect, int width, int height);
void scene_rect_set_color(struct flame_rect *rect, const float color[4]);
// NULL or an empty box shows all of it
void scene_surface_set_clip(struct flame_surface_node *node, const struct wlr_box *clip);
// a new picture: texture (NULL for none) drawn at width x height, readable
// once wait_timeline reaches wait_point (NULL: right away)
void scene_buffer_set(struct flame_buffer_node *node, struct wlr_texture *texture,
	const void *key, int width, int height, bool opaque,
	struct wlr_drm_syncobj_timeline *wait_timeline, uint64_t wait_point);
// only part of the texture, NULL for all of it again
void scene_buffer_set_source(struct flame_buffer_node *node, const struct wlr_fbox *src);

struct flame_tree *scene_tree_from_node(struct flame_node *node);

// topmost enabled node under a layout point. surface/sx/sy are set when it's
// client content that takes input there, NULL for rects (borders, backdrops).
// buffer nodes come back with surface NULL and sx/sy inside them
struct flame_node *scene_node_at(struct flame_scene *scene, double lx, double ly,
	struct wlr_surface **surface, double *sx, double *sy);

#endif
