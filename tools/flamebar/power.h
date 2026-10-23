#ifndef FLAMEBAR_POWER_H
#define FLAMEBAR_POWER_H

#include "../flameui/flameui.h"
#include "battery.h"

// the battery as the bar shows it (a laptop's; a desktop shows nothing):
// an icon as full as it is in the bar, and a card under it when it's
// clicked: how full, on what, for how long

#define POWER_CARD_W 250
#define POWER_CARD_H 100

// the icon and how full it is, its right edge at right: how wide
double power_icon(struct fui *ui, const struct battery_now *now, double right, double cy);
// the card at x, y, its glass into panes (from *n, as many as fit in max)
void power_card(struct fui *ui, const struct battery_now *now, double x, double y,
	struct fui_pane *panes, int *n, int max);

#endif
