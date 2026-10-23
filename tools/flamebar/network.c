#include "network.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// networkmanager's own properties (wi-fi on, the primary connection, its
// devices), then the primary connection's name and kind, then each
// device's kind: asked again whenever it says something changed

#define NM "org.freedesktop.NetworkManager"
#define NM_PATH "/org/freedesktop/NetworkManager"

struct network {
	sd_bus *bus;
	void (*changed)(void *data);
	void *data;
	sd_bus_slot *signal, *owner;
	bool have;
	struct network_now now;
	unsigned generation; // replies from an older ask are dropped
};

// a reply to an ask of this generation
struct ask {
	struct network *n;
	unsigned generation;
};

static void refresh(struct network *n);

static struct ask *ask_new(struct network *n) {
	struct ask *a = malloc(sizeof(*a));
	if (a) {
		*a = (struct ask){n, n->generation};
	}
	return a;
}

static bool current(struct ask *a, sd_bus_message *reply) {
	return a->generation == a->n->generation && !sd_bus_message_is_method_error(reply, NULL);
}

static int got_type(sd_bus_message *reply, void *data, sd_bus_error *error) {
	struct ask *a = data;
	uint32_t type;
	if (current(a, reply) && sd_bus_message_read(reply, "v", "u", &type) >= 0 && type == 2) {
		a->n->now.wifi = true; // NM_DEVICE_TYPE_WIFI
		a->n->changed(a->n->data);
	}
	free(a);
	return 0;
}

// the primary connection's Id and Type
static int got_connection(sd_bus_message *reply, void *data, sd_bus_error *error) {
	struct ask *a = data;
	struct network *n = a->n;
	if (current(a, reply) && sd_bus_message_enter_container(reply, 'a', "{sv}") >= 0) {
		while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
			const char *key, *value;
			if (sd_bus_message_read(reply, "s", &key) < 0) {
				break;
			}
			if ((!strcmp(key, "Id") || !strcmp(key, "Type"))
					&& sd_bus_message_read(reply, "v", "s", &value) >= 0) {
				if (!strcmp(key, "Id")) {
					snprintf(n->now.name, sizeof(n->now.name), "%s", value);
				} else {
					n->now.wireless = !strcmp(value, "802-11-wireless");
				}
			} else {
				sd_bus_message_skip(reply, "v");
			}
			sd_bus_message_exit_container(reply);
		}
		n->changed(n->data);
	}
	free(a);
	return 0;
}

static int got_manager(sd_bus_message *reply, void *data, sd_bus_error *error) {
	struct ask *a = data;
	struct network *n = a->n;
	if (!current(a, reply) || sd_bus_message_enter_container(reply, 'a', "{sv}") < 0) {
		if (a->generation == n->generation) {
			n->have = false;
			n->changed(n->data);
		}
		free(a);
		return 0;
	}
	n->have = true;
	n->now = (struct network_now){0};
	while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
		const char *key;
		if (sd_bus_message_read(reply, "s", &key) < 0) {
			break;
		}
		if (!strcmp(key, "WirelessEnabled")) {
			int on;
			sd_bus_message_read(reply, "v", "b", &on);
			n->now.wifi_on = on;
		} else if (!strcmp(key, "PrimaryConnection")) {
			const char *path;
			sd_bus_message_read(reply, "v", "o", &path);
			n->now.connected = strcmp(path, "/");
			struct ask *b = n->now.connected ? ask_new(n) : NULL;
			if (b && sd_bus_call_method_async(n->bus, NULL, NM, path,
					"org.freedesktop.DBus.Properties", "GetAll", got_connection, b, "s",
					NM ".Connection.Active") < 0) {
				free(b);
			}
		} else if (!strcmp(key, "Devices") && sd_bus_message_enter_container(reply, 'v', "ao") >= 0) {
			if (sd_bus_message_enter_container(reply, 'a', "o") >= 0) {
				const char *path;
				while (sd_bus_message_read(reply, "o", &path) > 0) {
					struct ask *b = ask_new(n);
					if (b && sd_bus_call_method_async(n->bus, NULL, NM, path,
							"org.freedesktop.DBus.Properties", "Get", got_type, b, "ss",
							NM ".Device", "DeviceType") < 0) {
						free(b);
					}
				}
				sd_bus_message_exit_container(reply);
			}
			sd_bus_message_exit_container(reply);
		} else {
			sd_bus_message_skip(reply, "v");
		}
		sd_bus_message_exit_container(reply);
	}
	n->changed(n->data);
	free(a);
	return 0;
}

static void refresh(struct network *n) {
	n->generation++;
	struct ask *a = ask_new(n);
	// not started for us (it's bus activated): the bar only looks
	sd_bus_message *call = NULL;
	if (a && (sd_bus_message_new_method_call(n->bus, &call, NM, NM_PATH,
			"org.freedesktop.DBus.Properties", "GetAll") < 0
			|| sd_bus_message_append(call, "s", NM) < 0
			|| sd_bus_message_set_auto_start(call, 0) < 0
			|| sd_bus_call_async(n->bus, NULL, call, got_manager, a, 0) < 0)) {
		free(a);
	}
	sd_bus_message_unref(call);
}

static int something_changed(sd_bus_message *msg, void *data, sd_bus_error *error) {
	refresh(data);
	return 0;
}

struct network *network_start(sd_bus *bus, void (*changed)(void *data), void *data) {
	if (!bus) {
		return NULL;
	}
	struct network *n = calloc(1, sizeof(*n));
	if (!n) {
		return NULL;
	}
	n->bus = bus;
	n->changed = changed;
	n->data = data;
	// its own properties changing, and it coming and going
	sd_bus_match_signal_async(bus, &n->signal, NM, NM_PATH, "org.freedesktop.DBus.Properties",
		"PropertiesChanged", something_changed, NULL, n);
	sd_bus_add_match_async(bus, &n->owner, "type='signal',sender='org.freedesktop.DBus',"
		"member='NameOwnerChanged',arg0='" NM "'", something_changed, NULL, n);
	refresh(n);
	return n;
}

void network_stop(struct network *n) {
	if (!n) {
		return;
	}
	sd_bus_slot_unref(n->signal);
	sd_bus_slot_unref(n->owner);
	free(n);
}

bool network_get(struct network *n, struct network_now *now) {
	if (!n || !n->have) {
		return false;
	}
	*now = n->now;
	return true;
}

void network_set_wifi(struct network *n, bool on) {
	if (!n || !n->have) {
		return;
	}
	sd_bus_call_method_async(n->bus, NULL, NM, NM_PATH, "org.freedesktop.DBus.Properties",
		"Set", NULL, NULL, "ssv", NM, "WirelessEnabled", "b", on);
	n->now.wifi_on = on; // shown now, confirmed when it says so
	sd_bus_flush(n->bus);
}
