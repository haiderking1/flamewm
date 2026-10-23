#ifndef FLAMEBAR_BLUETOOTH_H
#define FLAMEBAR_BLUETOOTH_H

#include <stdbool.h>
#include <systemd/sd-bus.h>

// bluetooth, from bluez (system bus): the adapter on or off and what's
// connected to it. changed called when that does

struct bluetooth;

struct bluetooth_now {
	bool on;
	char device[128]; // the one connected, "" for none
};

struct bluetooth *bluetooth_start(sd_bus *bus, void (*changed)(void *data), void *data);
void bluetooth_stop(struct bluetooth *bluetooth);

// false without bluez or an adapter
bool bluetooth_get(struct bluetooth *bluetooth, struct bluetooth_now *now);
void bluetooth_set_on(struct bluetooth *bluetooth, bool on);

#endif
