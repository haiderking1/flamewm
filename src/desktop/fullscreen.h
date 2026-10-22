#ifndef FLAME_DESKTOP_FULLSCREEN_H
#define FLAME_DESKTOP_FULLSCREEN_H

#include <stdbool.h>
#include <wlr/util/box.h>

struct flame_desktop;
struct flame_view;

// one fullscreen window per workspace, covering the whole
// output over black. it only shows up as fullscreen once the client commits
// the matching size, through the same transaction everything else uses

// client or user asks. false when refused: an unfocused window can't grab
// the screen. unmapped windows just remember it until they map
bool fullscreen_request(struct flame_desktop *desktop, struct flame_view *view,
	bool fullscreen);
void fullscreen_toggle(struct flame_desktop *desktop);

// a window that asked before it mapped (or got moved here) claims its workspace
void fullscreen_claim_if_wanted(struct flame_desktop *desktop, struct flame_view *view);

// black behind fullscreen windows, call whenever placement changed
void fullscreen_refresh_backdrops(struct flame_desktop *desktop);

// the whole output, bar reservations don't apply. false with no output
bool fullscreen_area(struct flame_desktop *desktop, struct wlr_box *box);

#endif
