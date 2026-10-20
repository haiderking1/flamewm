#ifndef FLAME_ANIM_WORKSPACE_H
#define FLAME_ANIM_WORKSPACE_H

#include <stdbool.h>

struct flame_desktop;
struct flame_workspace;

// switching workspaces: the old one slides out as the new one slides in,
// sideways (or up and down), the way the numbers go. the old one stays
// shown until it's out of sight

// from just became hidden, to just became the active one. false: no
// animation, from is hidden as usual
bool workspace_anim_switch(struct flame_desktop *desktop, struct flame_workspace *from,
	struct flame_workspace *to);
// whatever's sliding lands now
void workspace_anim_finish(struct flame_desktop *desktop);

#endif
