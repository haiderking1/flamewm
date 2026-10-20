#ifndef FLAME_RENDER_DAMAGE_H
#define FLAME_RENDER_DAMAGE_H

#include <pixman.h>

struct flame_element_list;

// what has to be redrawn because things showed up, went away, moved, changed
// look or changed order between two frames, layout coords, added to out.
// what clients redrew inside themselves comes from their commits instead
void damage_diff(const struct flame_element_list *prev,
	const struct flame_element_list *cur, pixman_region32_t *out);

#endif
