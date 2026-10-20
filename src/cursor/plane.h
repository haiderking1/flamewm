#ifndef FLAME_CURSOR_PLANE_H
#define FLAME_CURSOR_PLANE_H

#include <stdbool.h>
#include <stdint.h>

#include "cursor/dumb.h"

struct wlr_output;
struct wlr_texture;

// one output's hardware cursor plane, fed by the cpu: the image is written
// into a dumb buffer and KMS moves it around on its own. nothing gets
// composited when the mouse moves, and windows can go straight to the display
// underneath. three buffers take turns: one on screen, one maybe queued
// behind a pending flip, and the one being written
struct flame_cursor_plane {
	struct wlr_output *output;
	bool usable; // a DRM output with a cursor plane that took our buffers
	struct flame_dumb_device device;
	struct flame_dumb_buffer *buffers[3];
	int next;

	// what's on it: an image and the version of it, NULL for nothing
	const void *shown;
	uint64_t shown_seq;
	int x, y; // where it was moved to, output buffer coords
	// changed since the last commit, KMS only picks it up with one
	bool dirty;
};

bool cursor_plane_init(struct flame_cursor_plane *plane, struct wlr_output *output);
void cursor_plane_finish(struct flame_cursor_plane *plane);

// put an image on the plane, key/seq say whether it's the one already there.
// false when the plane can't show it (too big, KMS refused): draw it instead
bool cursor_plane_show_pixels(struct flame_cursor_plane *plane, const void *key,
	uint64_t seq, const void *pixels, int width, int height, int stride,
	int hotspot_x, int hotspot_y);
bool cursor_plane_show_texture(struct flame_cursor_plane *plane, const void *key,
	uint64_t seq, struct wlr_texture *texture, int hotspot_x, int hotspot_y);
void cursor_plane_hide(struct flame_cursor_plane *plane);
// KMS lost track of it (vt switch): the next show sets it again, and a
// plane that refused an image before gets tried again
void cursor_plane_forget(struct flame_cursor_plane *plane);
// the hotspot's spot in output buffer coords. true when it actually moved
bool cursor_plane_move(struct flame_cursor_plane *plane, int x, int y);

#endif
