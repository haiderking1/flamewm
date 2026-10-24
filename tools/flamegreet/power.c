#include "power.h"

#include <stdio.h>
#include <string.h>
#include <systemd/sd-bus.h>

static const char *const asks[POWER_COUNT] = {"Suspend", "Reboot", "PowerOff"};

static sd_bus_message *call(sd_bus *bus, const char *method, sd_bus_error *error,
		const char *types, int arg) {
	sd_bus_message *reply = NULL;
	int r = types ? sd_bus_call_method(bus, "org.freedesktop.login1", "/org/freedesktop/login1",
			"org.freedesktop.login1.Manager", method, error, &reply, types, arg)
		: sd_bus_call_method(bus, "org.freedesktop.login1", "/org/freedesktop/login1",
			"org.freedesktop.login1.Manager", method, error, &reply, NULL);
	return r < 0 ? NULL : reply;
}

void power_check(bool can[POWER_COUNT]) {
	sd_bus *bus = NULL;
	for (int i = 0; i < POWER_COUNT; i++) {
		can[i] = false;
	}
	if (sd_bus_open_system(&bus) < 0) {
		return;
	}
	for (int i = 0; i < POWER_COUNT; i++) {
		char method[32];
		snprintf(method, sizeof(method), "Can%s", asks[i]);
		sd_bus_error error = SD_BUS_ERROR_NULL;
		sd_bus_message *reply = call(bus, method, &error, NULL, 0);
		const char *answer = NULL;
		if (reply && sd_bus_message_read(reply, "s", &answer) >= 0 && answer) {
			can[i] = !strcmp(answer, "yes") || !strcmp(answer, "challenge");
		}
		sd_bus_message_unref(reply);
		sd_bus_error_free(&error);
	}
	sd_bus_unref(bus);
}

bool power_do(enum power what) {
	sd_bus *bus = NULL;
	if (sd_bus_open_system(&bus) < 0) {
		return false;
	}
	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = call(bus, asks[what], &error, "b", 0);
	bool ok = reply != NULL;
	if (!ok) {
		fprintf(stderr, "flamegreet: logind won't %s: %s\n", asks[what],
			error.message ? error.message : "no answer");
	}
	sd_bus_message_unref(reply);
	sd_bus_error_free(&error);
	sd_bus_unref(bus);
	return ok;
}
