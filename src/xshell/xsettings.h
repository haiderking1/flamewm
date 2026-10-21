#ifndef FLAME_XSHELL_XSETTINGS_H
#define FLAME_XSHELL_XSETTINGS_H

#include <stdbool.h>

struct flame_xshell;

// what scale X apps draw at, when they draw at the screen's real pixels
// (x_zero_scaling): an XSETTINGS manager (_XSETTINGS_S0) telling GTK and
// friends Xft/DPI, Gdk/WindowScalingFactor and Gdk/UnscaledDPI, and Xft.dpi
// in the root's RESOURCE_MANAGER for Qt, Xft and the rest. the rest of
// RESOURCE_MANAGER stays the user's (xrdb). off: none of it, what was there
// before comes back
void xsettings_update(struct flame_xshell *xshell, bool on, double scale);

#endif
