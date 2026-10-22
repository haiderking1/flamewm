#include "render/damage.h"

#include <stdlib.h>
#include <string.h>

#include "render/elements.h"
#include "scene/surface.h"

static void add_box(pixman_region32_t *out, struct wlr_box box) {
	pixman_region32_union_rect(out, out, box.x, box.y, box.width, box.height);
}

static void add_overlap(pixman_region32_t *out, struct wlr_box a, struct wlr_box b) {
	struct wlr_box overlap;
	if (wlr_box_intersection(&overlap, &a, &b)) {
		add_box(out, overlap);
	}
}

static bool fbox_equal(const struct wlr_fbox *a, const struct wlr_fbox *b) {
	return a->x == b->x && a->y == b->y && a->width == b->width && a->height == b->height;
}

static bool looks_the_same(const struct flame_element *a, const struct flame_element *b) {
	if (!wlr_box_equal(&a->box, &b->box) || !wlr_box_equal(&a->clip, &b->clip)
			|| a->alpha != b->alpha) {
		return false;
	}
	if (a->type == FLAME_ELEMENT_RECT) {
		return !memcmp(a->color, b->color, sizeof(a->color));
	}
	if (a->type == FLAME_ELEMENT_TEXTURE) {
		return a->image == b->image;
	}
	if (a->type == FLAME_ELEMENT_GLASS) {
		return !memcmp(&a->glass, &b->glass, sizeof(a->glass));
	}
	// a new buffer comes with commit damage, that's handled by the commit
	return a->transform == b->transform && fbox_equal(&a->src, &b->src);
}

// the id may be a freed surface's address taken by a new one, so a surface
// only counts as the same if it says it was in the last frame
static bool same_surface(const struct flame_element *element) {
	if (element->type != FLAME_ELEMENT_SURFACE) {
		return true;
	}
	struct flame_surface_state *state = scene_surface_state(element->surface);
	return state && scene_surface_drawn(state);
}

void damage_diff(const struct flame_element_list *prev,
		const struct flame_element_list *cur, pixman_region32_t *out) {
	// where each current element was in prev, SIZE_MAX for new ones
	size_t *was = malloc((cur->len ? cur->len : 1) * sizeof(*was));
	bool *kept = calloc(prev->len ? prev->len : 1, sizeof(*kept));
	if (!was || !kept) {
		// can't tell, redraw everything both frames touched
		for (size_t i = 0; i < cur->len; i++) {
			add_box(out, cur->data[i].clip);
		}
		for (size_t i = 0; i < prev->len; i++) {
			add_box(out, prev->data[i].clip);
		}
		free(was);
		free(kept);
		return;
	}

	size_t offset = 0; // lists mostly shift by whole runs
	bool reordered = false;
	size_t last = 0;
	bool any = false;
	for (size_t i = 0; i < cur->len; i++) {
		const struct flame_element *element = &cur->data[i];
		size_t j;
		const struct flame_element *old = elements_find(prev, element->id, i + offset, &j);
		if (!old || old->type != element->type || !same_surface(element)) {
			was[i] = SIZE_MAX;
			add_box(out, element->clip);
			if (old) {
				// whatever had that address before is gone from there
				add_box(out, old->clip);
				kept[j] = true;
			}
			continue;
		}
		was[i] = j;
		kept[j] = true;
		offset = j - i;
		if (any && j < last) {
			reordered = true;
		}
		last = j;
		any = true;
		if (!looks_the_same(old, element)) {
			add_box(out, old->clip);
			add_box(out, element->clip);
		}
	}

	for (size_t j = 0; j < prev->len; j++) {
		if (!kept[j]) {
			add_box(out, prev->data[j].clip);
		}
	}

	// order only matters where two of them overlap: redraw those spots for
	// every pair that swapped. a focus change raising one window touches
	// just its popups' overlap, not the whole screen
	if (reordered) {
		for (size_t a = 0; a < cur->len; a++) {
			if (was[a] == SIZE_MAX) {
				continue;
			}
			for (size_t b = a + 1; b < cur->len; b++) {
				if (was[b] == SIZE_MAX || was[b] > was[a]) {
					continue;
				}
				add_overlap(out, cur->data[a].clip, cur->data[b].clip);
				add_overlap(out, prev->data[was[a]].clip, prev->data[was[b]].clip);
			}
		}
	}
	free(was);
	free(kept);
}
