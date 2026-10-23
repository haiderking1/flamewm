#ifndef FLAMEBAR_CALENDAR_H
#define FLAMEBAR_CALENDAR_H

#include "../flameui/flameui.h"

// the calendar, a card under the time when it's clicked: the day picked
// big (today's to start, how far from today it is, its week), and a
// month of days under it, today lit. other months with the arrows or the
// wheel, a day clicked is picked, "Today" goes back

#define CALENDAR_W 294
#define CALENDAR_H 350

struct calendar {
	int month; // the one showing: years * 12 + months
	// the one it slid from, which way (1: on to a later one), how many
	// slides there have been (each one's own animation)
	int from, dir;
	unsigned slides;
	int year, mon, mday; // the day picked
	double wheel; // the wheel's turn not yet a month's worth
};

// opened: today's month, today picked
void calendar_open(struct calendar *c);
// the card at x, y, its glass into panes (from *n, as many as fit in max).
// live: it takes the pointer (not while it fades)
void calendar_card(struct fui *ui, struct calendar *c, double x, double y,
	struct fui_pane *panes, int *n, int max, bool live);

#endif
