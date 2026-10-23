#include "bus.h"

#include <stdio.h>

static void process(sd_bus *bus) {
	while (sd_bus_process(bus, NULL) > 0) {
	}
}

static void readable(struct fui *ui, int fd, void *data) {
	process(data);
}

// replies that never came time out: looked at every second
static void tick(struct fui *ui, void *data) {
	process(data);
	fui_after(ui, 1000, tick, data);
}

sd_bus *bus_open(struct fui *ui, bool system) {
	sd_bus *bus = NULL;
	int r = system ? sd_bus_open_system(&bus) : sd_bus_open_user(&bus);
	if (r < 0 || !fui_watch(ui, sd_bus_get_fd(bus), readable, bus)) {
		fprintf(stderr, "flamebar: no %s bus\n", system ? "system" : "session");
		sd_bus_unref(bus);
		return NULL;
	}
	fui_after(ui, 1000, tick, bus);
	return bus;
}

void bus_flush(sd_bus *bus) {
	if (bus) {
		sd_bus_flush(bus);
		process(bus);
	}
}
