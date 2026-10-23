#include "battery.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UPOWER "org.freedesktop.UPower"
#define DISPLAY "/org/freedesktop/UPower/devices/DisplayDevice"
#define DEVICE UPOWER ".Device"
#define SYSFS "/sys/class/power_supply"

enum source { NONE, UPOWERD, KERNEL };

struct battery {
	sd_bus *bus;
	void (*changed)(void *data);
	void *data;
	sd_bus_slot *signal, *owner;
	enum source source;
	bool have;
	struct battery_now now;
};

// ---- the kernel's files, without upower ----

static bool read_line(const char *dir, const char *file, char *out, size_t size) {
	char path[512];
	snprintf(path, sizeof(path), SYSFS "/%s/%s", dir, file);
	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}
	bool got = fgets(out, (int)size, f) != NULL;
	fclose(f);
	if (got) {
		out[strcspn(out, "\n")] = '\0';
	}
	return got;
}

// the system's batteries put together (a laptop can have two). one a
// device has of its own (a mouse's, scope "Device") isn't the computer's
static bool from_kernel(struct battery *b) {
	DIR *dir = opendir(SYSFS);
	if (!dir) {
		return false;
	}
	double now = 0, full = 0;
	int count = 0;
	bool charging = false, discharging = false;
	struct dirent *e;
	while ((e = readdir(dir))) {
		char type[32], scope[32], status[32], value[32];
		if (e->d_name[0] == '.' || !read_line(e->d_name, "type", type, sizeof(type))
				|| strcmp(type, "Battery")
				|| (read_line(e->d_name, "scope", scope, sizeof(scope))
					&& !strcmp(scope, "Device"))) {
			continue;
		}
		double capacity = read_line(e->d_name, "capacity", value, sizeof(value))
			? atof(value) : -1;
		if (capacity < 0) {
			continue;
		}
		now += capacity;
		full += 100;
		count++;
		if (read_line(e->d_name, "status", status, sizeof(status))) {
			charging |= !strcmp(status, "Charging");
			discharging |= !strcmp(status, "Discharging");
		}
	}
	closedir(dir);
	if (!count) {
		return false;
	}
	b->now = (struct battery_now){
		.percent = 100 * now / full,
		.charging = charging,
		.plugged = !discharging,
		.full = !charging && !discharging,
	};
	return true;
}

// ---- upower ----

static int got_all(sd_bus_message *reply, void *data, sd_bus_error *error) {
	struct battery *b = data;
	if (sd_bus_message_is_method_error(reply, NULL)) {
		// no upower: the kernel's files
		b->source = KERNEL;
		b->have = from_kernel(b);
		b->changed(b->data);
		return 0;
	}
	bool present = false;
	uint32_t type = 0, state = 0;
	double percent = 0;
	int64_t to_empty = 0, to_full = 0;
	if (sd_bus_message_enter_container(reply, 'a', "{sv}") < 0) {
		return 0;
	}
	while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
		const char *key;
		if (sd_bus_message_read(reply, "s", &key) < 0) {
			break;
		}
		int is_present;
		if (!strcmp(key, "IsPresent") && sd_bus_message_read(reply, "v", "b", &is_present) >= 0) {
			present = is_present;
		} else if (!strcmp(key, "Type")) {
			sd_bus_message_read(reply, "v", "u", &type);
		} else if (!strcmp(key, "State")) {
			sd_bus_message_read(reply, "v", "u", &state);
		} else if (!strcmp(key, "Percentage")) {
			sd_bus_message_read(reply, "v", "d", &percent);
		} else if (!strcmp(key, "TimeToEmpty")) {
			sd_bus_message_read(reply, "v", "x", &to_empty);
		} else if (!strcmp(key, "TimeToFull")) {
			sd_bus_message_read(reply, "v", "x", &to_full);
		} else {
			sd_bus_message_skip(reply, "v");
		}
		sd_bus_message_exit_container(reply);
	}
	sd_bus_message_exit_container(reply);
	b->source = UPOWERD;
	// its display device stands for the system's batteries: a desktop's
	// isn't there, or isn't a battery (type 2)
	b->have = present && type == 2;
	// states: 1 charging, 2 discharging, 4 full, 5 plugged in and held back
	b->now = (struct battery_now){
		.percent = percent,
		.charging = state == 1,
		.plugged = state == 1 || state == 4 || state == 5,
		.full = state == 4,
		.seconds = (double)(state == 1 ? to_full : state == 2 ? to_empty : 0),
	};
	b->changed(b->data);
	return 0;
}

static void ask(struct battery *b) {
	// not started for us (it's bus activated): the bar only looks
	sd_bus_message *call = NULL;
	if (sd_bus_message_new_method_call(b->bus, &call, UPOWER, DISPLAY,
			"org.freedesktop.DBus.Properties", "GetAll") >= 0
			&& sd_bus_message_append(call, "s", DEVICE) >= 0
			&& sd_bus_message_set_auto_start(call, 0) >= 0) {
		sd_bus_call_async(b->bus, NULL, call, got_all, b, 0);
	}
	sd_bus_message_unref(call);
}

static int something_changed(sd_bus_message *msg, void *data, sd_bus_error *error) {
	ask(data);
	return 0;
}

struct battery *battery_start(sd_bus *system, void (*changed)(void *data), void *data) {
	struct battery *b = calloc(1, sizeof(*b));
	if (!b) {
		return NULL;
	}
	b->changed = changed;
	b->data = data;
	if (!system) {
		b->source = KERNEL;
		b->have = from_kernel(b);
		return b;
	}
	b->bus = sd_bus_ref(system);
	sd_bus_match_signal_async(system, &b->signal, UPOWER, DISPLAY,
		"org.freedesktop.DBus.Properties", "PropertiesChanged", something_changed, NULL, b);
	sd_bus_add_match_async(system, &b->owner, "type='signal',sender='org.freedesktop.DBus',"
		"member='NameOwnerChanged',arg0='" UPOWER "'", something_changed, NULL, b);
	ask(b);
	return b;
}

void battery_stop(struct battery *b) {
	if (!b) {
		return;
	}
	sd_bus_slot_unref(b->signal);
	sd_bus_slot_unref(b->owner);
	sd_bus_unref(b->bus);
	free(b);
}

bool battery_get(struct battery *b, struct battery_now *now) {
	if (!b || !b->have) {
		return false;
	}
	*now = b->now;
	return true;
}

void battery_refresh(struct battery *b) {
	if (b && b->source == KERNEL) {
		bool had = b->have;
		b->have = from_kernel(b);
		if (had || b->have) {
			b->changed(b->data);
		}
	}
}
