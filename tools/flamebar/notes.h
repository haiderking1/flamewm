#ifndef FLAMEBAR_NOTES_H
#define FLAMEBAR_NOTES_H

#include "../flameui/flameui.h"
#include "notify.h"

// notifications as the bar shows them: cards under it at the right as they
// come, gone after a while (pointed at, they stay), and the list of them
// all that opens under the time

#define NOTES_W 340

struct notes {
	struct notify *notify;
	double scroll; // the list's, how far down it is
	// where each card's place in the stack is: one going stays there
	struct place {
		uint32_t id;
		double y;
		bool entered; // on its way in or there: it doesn't wait for room
	} places[64];
	int places_len;
	// the list's this frame: how tall, each one's height, how much room it
	// takes (0 to 1) and how far it's slid out
	double list_h, heights[64], ins[64], slides[64];
	// this frame, the ones still being taken away (closed, sliding out)
	uint32_t busy[64];
	int busy_len;
};

// the cards: their right edge at right, from top down. hidden (the list or
// the control center open), they go and wait. their glass into panes and
// where they take clicks into rects (each from *n up, as many as fit in
// max); how far down they reach now, and in want how far down they're
// going (0 for none)
double notes_cards(struct fui *ui, struct notes *notes, double right, double top, bool hidden,
	struct fui_pane *panes, int *n_panes, int max_panes, struct fui_rect *rects, int *n_rects,
	int max_rects, double *want);

// the list: how tall it is (no more than max), and drawn with its top
// right at right, top (its glass into panes as above)
double notes_list_height(struct fui *ui, struct notes *notes, double max);
void notes_list(struct fui *ui, struct notes *notes, double right, double top, double max,
	struct fui_pane *panes, int *n_panes, int max_panes);

// after both: the closed ones nothing's taking away any more, let go
void notes_done(struct notes *notes);

#endif
