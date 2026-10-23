#ifndef FLAMEBAR_BACKLIGHT_H
#define FLAMEBAR_BACKLIGHT_H

#include <systemd/sd-bus.h>

#include <stdbool.h>

// how bright the screen is: a laptop's panel (its backlight, through
// logind), else the monitors on cables (asked over the cable, ddc/ci, on a
// thread of its own: it's slow). see backlight.c

struct backlight;

struct backlight *backlight_start(sd_bus *system);
void backlight_stop(struct backlight *backlight);
// there's something to make brighter (monitors: once they've been looked for)
bool backlight_there(struct backlight *backlight);
double backlight_get(struct backlight *backlight); // 0 to 1
void backlight_set(struct backlight *backlight, double level);

#endif
