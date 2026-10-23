#ifndef FLAMEBAR_BACKLIGHT_H
#define FLAMEBAR_BACKLIGHT_H

#include <systemd/sd-bus.h>

// the screen's backlight (a laptop's): how bright it is, set through
// logind (no root, no udev rule). a monitor on a cable has none

struct backlight;

struct backlight *backlight_start(sd_bus *system); // NULL when there's none
void backlight_stop(struct backlight *backlight);
double backlight_get(struct backlight *backlight); // 0 to 1
void backlight_set(struct backlight *backlight, double level);

#endif
