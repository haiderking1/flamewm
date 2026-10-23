#ifndef FLAMEBAR_NETWORK_H
#define FLAMEBAR_NETWORK_H

#include <stdbool.h>
#include <systemd/sd-bus.h>

// the network, from networkmanager (system bus): what it's connected
// through and whether there's wi-fi to turn on and off. changed called when
// it does

struct network;

struct network_now {
	bool wifi; // there's a wi-fi device
	bool wifi_on;
	bool connected;
	bool wireless; // connected through wi-fi
	char name[128]; // the connection's
};

struct network *network_start(sd_bus *bus, void (*changed)(void *data), void *data);
void network_stop(struct network *network);

// false without networkmanager
bool network_get(struct network *network, struct network_now *now);
void network_set_wifi(struct network *network, bool on);

#endif
