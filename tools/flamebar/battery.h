#ifndef FLAMEBAR_BATTERY_H
#define FLAMEBAR_BATTERY_H

#include <stdbool.h>
#include <systemd/sd-bus.h>

// the computer's own battery (a laptop's): how full, charging or not, how
// long it lasts. upower's word for it (its "display device": the system's
// batteries, never a mouse's or a headset's), else the kernel's own files
// for them. a computer without one has none: nothing's ever shown for it

struct battery_now {
	double percent; // 0 to 100
	bool plugged; // on the adapter (charging, or full, or held back)
	bool charging;
	bool full;
	double seconds; // till empty, or till full while charging; 0 for unknown
};

struct battery;

struct battery *battery_start(sd_bus *system, void (*changed)(void *data), void *data);
void battery_stop(struct battery *battery);
// false: there's no battery (a desktop)
bool battery_get(struct battery *battery, struct battery_now *now);
// the kernel's files read again (without upower they don't say they changed)
void battery_refresh(struct battery *battery);

#endif
