#ifndef FLAMEBAR_BUS_H
#define FLAMEBAR_BUS_H

#include <systemd/sd-bus.h>

#include "../flameui/flameui.h"

// a d-bus connection run by the bar's loop: its messages handled as they
// come, its timeouts on a timer. the session's (players) and the system's
// (network, bluetooth). NULL when there's none

sd_bus *bus_open(struct fui *ui, bool system);
// what was just sent out now, not when the next message comes
void bus_flush(sd_bus *bus);

#endif
