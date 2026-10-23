#include "bluetooth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// all of bluez's objects at once (its object manager): the first adapter
// and the devices connected. asked again whenever bluez says anything
// changed, or comes and goes

#define BLUEZ "org.bluez"

struct bluetooth {
	sd_bus *bus;
	void (*changed)(void *data);
	void *data;
	sd_bus_slot *properties, *added, *removed, *owner;
	bool have;
	struct bluetooth_now now;
	char adapter[128]; // its object path
	unsigned generation;
};

struct ask {
	struct bluetooth *b;
	unsigned generation;
};

// one object's interfaces (a{sa{sv}}): an adapter's Powered, a device's
// Connected and Alias
static void read_object(struct bluetooth *b, sd_bus_message *msg, const char *path) {
	if (sd_bus_message_enter_container(msg, 'a', "{sa{sv}}") < 0) {
		return;
	}
	while (sd_bus_message_enter_container(msg, 'e', "sa{sv}") > 0) {
		const char *iface;
		sd_bus_message_read(msg, "s", &iface);
		bool adapter = !strcmp(iface, BLUEZ ".Adapter1"), device = !strcmp(iface,
			BLUEZ ".Device1");
		bool connected = false, powered = false;
		char alias[128] = "";
		if (sd_bus_message_enter_container(msg, 'a', "{sv}") >= 0) {
			while (sd_bus_message_enter_container(msg, 'e', "sv") > 0) {
				const char *key;
				sd_bus_message_read(msg, "s", &key);
				int flag;
				const char *text;
				if ((adapter && !strcmp(key, "Powered"))
						|| (device && !strcmp(key, "Connected"))) {
					sd_bus_message_read(msg, "v", "b", &flag);
					*(adapter ? &powered : &connected) = flag;
				} else if (device && !strcmp(key, "Alias")
						&& sd_bus_message_read(msg, "v", "s", &text) >= 0) {
					snprintf(alias, sizeof(alias), "%s", text);
				} else {
					sd_bus_message_skip(msg, "v");
				}
				sd_bus_message_exit_container(msg);
			}
			sd_bus_message_exit_container(msg);
		}
		if (adapter && !b->adapter[0]) {
			snprintf(b->adapter, sizeof(b->adapter), "%s", path);
			b->now.on = powered;
			b->have = true;
		}
		if (device && connected && !b->now.device[0]) {
			snprintf(b->now.device, sizeof(b->now.device), "%s", alias);
		}
		sd_bus_message_exit_container(msg);
	}
	sd_bus_message_exit_container(msg);
}

static int got_objects(sd_bus_message *reply, void *data, sd_bus_error *error) {
	struct ask *a = data;
	struct bluetooth *b = a->b;
	if (a->generation != b->generation) {
		free(a);
		return 0;
	}
	b->have = false;
	b->adapter[0] = '\0';
	b->now = (struct bluetooth_now){0};
	if (!sd_bus_message_is_method_error(reply, NULL)
			&& sd_bus_message_enter_container(reply, 'a', "{oa{sa{sv}}}") >= 0) {
		while (sd_bus_message_enter_container(reply, 'e', "oa{sa{sv}}") > 0) {
			const char *path;
			sd_bus_message_read(reply, "o", &path);
			read_object(b, reply, path);
			sd_bus_message_exit_container(reply);
		}
		sd_bus_message_exit_container(reply);
	}
	b->changed(b->data);
	free(a);
	return 0;
}

static int refresh(sd_bus_message *msg, void *data, sd_bus_error *error) {
	struct bluetooth *b = data;
	b->generation++;
	struct ask *a = malloc(sizeof(*a));
	if (!a) {
		return 0;
	}
	*a = (struct ask){b, b->generation};
	// not started for us (bluez is bus activated): the bar only looks
	sd_bus_message *call = NULL;
	if (sd_bus_message_new_method_call(b->bus, &call, BLUEZ, "/",
			"org.freedesktop.DBus.ObjectManager", "GetManagedObjects") < 0
			|| sd_bus_message_set_auto_start(call, 0) < 0
			|| sd_bus_call_async(b->bus, NULL, call, got_objects, a, 0) < 0) {
		free(a);
	}
	sd_bus_message_unref(call);
	return 0;
}

struct bluetooth *bluetooth_start(sd_bus *bus, void (*changed)(void *data), void *data) {
	if (!bus) {
		return NULL;
	}
	struct bluetooth *b = calloc(1, sizeof(*b));
	if (!b) {
		return NULL;
	}
	b->bus = bus;
	b->changed = changed;
	b->data = data;
	sd_bus_add_match_async(bus, &b->properties, "type='signal',sender='" BLUEZ "',"
		"interface='org.freedesktop.DBus.Properties',member='PropertiesChanged'", refresh, NULL,
		b);
	sd_bus_add_match_async(bus, &b->added, "type='signal',sender='" BLUEZ "',"
		"member='InterfacesAdded'", refresh, NULL, b);
	sd_bus_add_match_async(bus, &b->removed, "type='signal',sender='" BLUEZ "',"
		"member='InterfacesRemoved'", refresh, NULL, b);
	sd_bus_add_match_async(bus, &b->owner, "type='signal',sender='org.freedesktop.DBus',"
		"member='NameOwnerChanged',arg0='" BLUEZ "'", refresh, NULL, b);
	refresh(NULL, b, NULL);
	return b;
}

void bluetooth_stop(struct bluetooth *b) {
	if (!b) {
		return;
	}
	sd_bus_slot_unref(b->properties);
	sd_bus_slot_unref(b->added);
	sd_bus_slot_unref(b->removed);
	sd_bus_slot_unref(b->owner);
	free(b);
}

bool bluetooth_get(struct bluetooth *b, struct bluetooth_now *now) {
	if (!b || !b->have) {
		return false;
	}
	*now = b->now;
	return true;
}

void bluetooth_set_on(struct bluetooth *b, bool on) {
	if (!b || !b->have) {
		return;
	}
	sd_bus_call_method_async(b->bus, NULL, BLUEZ, b->adapter, "org.freedesktop.DBus.Properties",
		"Set", NULL, NULL, "ssv", BLUEZ ".Adapter1", "Powered", "b", on);
	b->now.on = on;
	sd_bus_flush(b->bus);
}
