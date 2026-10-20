#ifndef FLAME_RENDER_SCANOUT_H
#define FLAME_RENDER_SCANOUT_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wlr/util/box.h>

struct flame_element;
struct flame_element_list;
struct flame_render;
struct flame_render_target;
struct wlr_drm_syncobj_timeline;
struct wlr_output;
struct wlr_output_state;
struct wlr_surface;

// direct scanout: when one fullscreen client buffer is all that shows, KMS
// gets that buffer itself. no composition, no gpu work from us, and the
// client's frame reaches the screen as early as it can
struct flame_scanout {
	struct flame_render *render;
	struct wlr_output *output;
	bool active; // the last frame went out this way

	// clients only allocate buffers KMS can take when told to. the hint
	// follows the candidate after it held for a while, so a window that's
	// only fullscreen for a moment doesn't make the client reallocate
	int debounce;
	struct wlr_surface *hinted;
	struct wl_listener hinted_destroy;
};

void scanout_init(struct flame_scanout *scanout, struct flame_render *render,
	struct wlr_output *output);
void scanout_finish(struct flame_scanout *scanout);

// the frame's one visible thing, a client surface covering all of area,
// else NULL
const struct flame_element *scanout_candidate(const struct flame_element_list *list,
	struct wlr_box area);
// once per frame with the candidate (or NULL), for the dmabuf hints
void scanout_hint(struct flame_scanout *scanout, const struct flame_element *candidate);

// puts the candidate's buffer in state if KMS takes it (a test commit
// decides). out_timeline/out_point are where KMS says it's done with it,
// the client gets its buffer back from there. false: compose instead
bool scanout_try(struct flame_scanout *scanout, const struct flame_element *candidate,
	const struct flame_render_target *target, struct wlr_output_state *state,
	struct wlr_drm_syncobj_timeline *out_timeline, uint64_t *out_point);

#endif
