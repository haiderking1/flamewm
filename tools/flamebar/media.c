#include "media.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// every org.mpris.MediaPlayer2.* on the session bus (but playerctld, it
// only mirrors the others): its status and what it plays, kept up to date
// by its PropertiesChanged. a player is told apart by its unique name, the
// sender of what it says

#define PLAYERS 16
#define PREFIX "org.mpris.MediaPlayer2."
#define PATH "/org/mpris/MediaPlayer2"
#define PLAYER_IFACE "org.mpris.MediaPlayer2.Player"

struct player {
	bool used;
	char name[128]; // its well-known name
	char owner[64]; // its unique one, "" till known
	bool playing;
	char title[256], artist[256], art[PATH_MAX];
	double changed_at;
	// how long, where it was at position_at (it moves on while it plays),
	// the track's id (what moving in it names)
	double length, position, position_at;
	char track[256];
	bool can_seek;
};

struct media {
	sd_bus *bus;
	void (*changed)(void *data);
	void *data;
	sd_bus_slot *owners, *properties, *seeked;
	struct player players[PLAYERS];
};

static double now(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static struct player *by_name(struct media *m, const char *name) {
	for (int i = 0; i < PLAYERS; i++) {
		if (m->players[i].used && !strcmp(m->players[i].name, name)) {
			return &m->players[i];
		}
	}
	return NULL;
}

static struct player *by_owner(struct media *m, const char *owner) {
	for (int i = 0; owner && i < PLAYERS; i++) {
		if (m->players[i].used && !strcmp(m->players[i].owner, owner)) {
			return &m->players[i];
		}
	}
	return NULL;
}

// a file:// url's path, %xx undone. "" for anything else (art online isn't
// fetched)
static void url_path(const char *url, char *out, size_t size) {
	out[0] = '\0';
	if (strncmp(url, "file://", 7)) {
		return;
	}
	size_t n = 0;
	for (const char *c = url + 7; *c && n + 1 < size; c++) {
		unsigned hex;
		if (*c == '%' && c[1] && c[2] && sscanf(c + 1, "%2x", &hex) == 1) {
			out[n++] = (char)hex;
			c += 2;
		} else {
			out[n++] = *c;
		}
	}
	out[n] = '\0';
}

// a variant holding a string, or an array of them (the first): into out
static void read_text(sd_bus_message *msg, char *out, size_t size) {
	char type;
	const char *contents;
	if (sd_bus_message_peek_type(msg, &type, &contents) < 0) {
		return;
	}
	const char *s = NULL;
	if (!strcmp(contents, "s") && sd_bus_message_read(msg, "v", "s", &s) >= 0) {
		snprintf(out, size, "%s", s);
	} else if (!strcmp(contents, "as") && sd_bus_message_enter_container(msg, 'v', "as") >= 0) {
		if (sd_bus_message_enter_container(msg, 'a', "s") >= 0) {
			if (sd_bus_message_read(msg, "s", &s) > 0) {
				snprintf(out, size, "%s", s);
			}
			while (sd_bus_message_skip(msg, "s") > 0) {
			}
			sd_bus_message_exit_container(msg);
		}
		sd_bus_message_exit_container(msg);
	} else {
		sd_bus_message_skip(msg, "v");
	}
}

// a variant holding a number of any kind (players differ), else 0
static double read_number(sd_bus_message *msg) {
	char type;
	const char *contents;
	if (sd_bus_message_peek_type(msg, &type, &contents) < 0 || strlen(contents) != 1) {
		sd_bus_message_skip(msg, "v");
		return 0;
	}
	union {
		int64_t x;
		uint64_t t;
		int32_t i;
		uint32_t u;
		double d;
	} v = {0};
	char sig[2] = {contents[0], '\0'};
	if (!strchr("xtiud", sig[0]) || sd_bus_message_read(msg, "v", sig, &v) < 0) {
		sd_bus_message_skip(msg, "v");
		return 0;
	}
	switch (sig[0]) {
	case 'x':
		return (double)v.x;
	case 't':
		return (double)v.t;
	case 'i':
		return v.i;
	case 'u':
		return v.u;
	default:
		return v.d;
	}
}

// where p is now: where it was, and on from there while it plays
static double position_now(const struct player *p) {
	double t = p->position + (p->playing ? now() - p->position_at : 0);
	return p->length > 0 && t > p->length ? p->length : t;
}

static void set_position(struct player *p, double t) {
	p->position = t < 0 ? 0 : t;
	p->position_at = now();
}

// Metadata (a{sv} in a variant): replaced whole
static void read_metadata(sd_bus_message *msg, struct player *p) {
	p->title[0] = p->artist[0] = p->art[0] = p->track[0] = '\0';
	p->length = 0;
	if (sd_bus_message_enter_container(msg, 'v', "a{sv}") < 0) {
		sd_bus_message_skip(msg, "v");
		return;
	}
	if (sd_bus_message_enter_container(msg, 'a', "{sv}") >= 0) {
		while (sd_bus_message_enter_container(msg, 'e', "sv") > 0) {
			const char *key;
			if (sd_bus_message_read(msg, "s", &key) < 0) {
				break;
			}
			if (!strcmp(key, "xesam:title")) {
				read_text(msg, p->title, sizeof(p->title));
			} else if (!strcmp(key, "xesam:artist")) {
				read_text(msg, p->artist, sizeof(p->artist));
			} else if (!strcmp(key, "mpris:artUrl")) {
				char url[PATH_MAX + 16] = "";
				read_text(msg, url, sizeof(url));
				url_path(url, p->art, sizeof(p->art));
			} else if (!strcmp(key, "mpris:length")) {
				p->length = read_number(msg) / 1e6;
			} else if (!strcmp(key, "mpris:trackid")) {
				// an object path by the spec, a string from some
				char type;
				const char *contents, *id = NULL;
				if (sd_bus_message_peek_type(msg, &type, &contents) >= 0
						&& (!strcmp(contents, "o") || !strcmp(contents, "s"))
						&& sd_bus_message_read(msg, "v", contents, &id) >= 0) {
					snprintf(p->track, sizeof(p->track), "%s", id);
				} else {
					sd_bus_message_skip(msg, "v");
				}
			} else {
				sd_bus_message_skip(msg, "v");
			}
			sd_bus_message_exit_container(msg);
		}
		sd_bus_message_exit_container(msg);
	}
	sd_bus_message_exit_container(msg);
}

// a player's properties (a{sv}): its status and what it plays
static void read_properties(sd_bus_message *msg, struct player *p) {
	if (sd_bus_message_enter_container(msg, 'a', "{sv}") < 0) {
		return;
	}
	while (sd_bus_message_enter_container(msg, 'e', "sv") > 0) {
		const char *key;
		if (sd_bus_message_read(msg, "s", &key) < 0) {
			break;
		}
		if (!strcmp(key, "PlaybackStatus")) {
			char status[32] = "";
			read_text(msg, status, sizeof(status));
			// where it got to before it changed, from there on
			set_position(p, position_now(p));
			p->playing = !strcmp(status, "Playing");
		} else if (!strcmp(key, "Position")) {
			set_position(p, read_number(msg) / 1e6);
		} else if (!strcmp(key, "CanSeek")) {
			int can = 0;
			sd_bus_message_read(msg, "v", "b", &can);
			p->can_seek = can;
		} else if (!strcmp(key, "Metadata")) {
			read_metadata(msg, p);
		} else {
			sd_bus_message_skip(msg, "v");
		}
		sd_bus_message_exit_container(msg);
	}
	sd_bus_message_exit_container(msg);
	p->changed_at = now();
}

static int got_all(sd_bus_message *reply, void *data, sd_bus_error *error) {
	struct media *m = data;
	const char *sender = sd_bus_message_get_sender(reply);
	struct player *p = by_owner(m, sender);
	if (!p || sd_bus_message_is_method_error(reply, NULL)) {
		return 0;
	}
	read_properties(reply, p);
	m->changed(m->data);
	return 0;
}

// an owner asked for: whose
struct ask {
	struct media *m;
	char name[128];
};

static int got_owner(sd_bus_message *reply, void *data, sd_bus_error *error);

// a player that's just there: its owner, then all it has
static void add(struct media *m, const char *name, const char *owner) {
	if (strncmp(name, PREFIX, strlen(PREFIX)) || !strcmp(name, PREFIX "playerctld")) {
		return;
	}
	struct player *p = by_name(m, name);
	for (int i = 0; !p && i < PLAYERS; i++) {
		p = m->players[i].used ? NULL : &m->players[i];
	}
	if (!p) {
		return;
	}
	*p = (struct player){.used = true, .changed_at = now()};
	snprintf(p->name, sizeof(p->name), "%s", name);
	if (owner && *owner) {
		snprintf(p->owner, sizeof(p->owner), "%s", owner);
		sd_bus_call_method_async(m->bus, NULL, name, PATH, "org.freedesktop.DBus.Properties",
			"GetAll", got_all, m, "s", PLAYER_IFACE);
	} else {
		struct ask *ask = malloc(sizeof(*ask));
		if (!ask) {
			return;
		}
		ask->m = m;
		snprintf(ask->name, sizeof(ask->name), "%s", name);
		if (sd_bus_call_method_async(m->bus, NULL, "org.freedesktop.DBus",
				"/org/freedesktop/DBus", "org.freedesktop.DBus", "GetNameOwner", got_owner, ask,
				"s", name) < 0) {
			free(ask);
		}
	}
}

static int got_owner(sd_bus_message *reply, void *data, sd_bus_error *error) {
	struct ask *ask = data;
	struct media *m = ask->m;
	const char *owner;
	struct player *p = by_name(m, ask->name);
	if (p && !sd_bus_message_is_method_error(reply, NULL)
			&& sd_bus_message_read(reply, "s", &owner) >= 0) {
		snprintf(p->owner, sizeof(p->owner), "%s", owner);
		sd_bus_call_method_async(m->bus, NULL, p->name, PATH, "org.freedesktop.DBus.Properties",
			"GetAll", got_all, m, "s", PLAYER_IFACE);
	}
	free(ask);
	return 0;
}

static int owner_changed(sd_bus_message *msg, void *data, sd_bus_error *error) {
	struct media *m = data;
	const char *name, *old, *owner;
	if (sd_bus_message_read(msg, "sss", &name, &old, &owner) < 0
			|| strncmp(name, PREFIX, strlen(PREFIX))) {
		return 0;
	}
	struct player *p = by_name(m, name);
	if (!*owner) {
		if (p) {
			p->used = false;
			m->changed(m->data);
		}
		return 0;
	}
	add(m, name, owner);
	return 0;
}

static int properties_changed(sd_bus_message *msg, void *data, sd_bus_error *error) {
	struct media *m = data;
	const char *iface;
	struct player *p = by_owner(m, sd_bus_message_get_sender(msg));
	if (!p || sd_bus_message_read(msg, "s", &iface) < 0 || strcmp(iface, PLAYER_IFACE)) {
		return 0;
	}
	read_properties(msg, p);
	m->changed(m->data);
	return 0;
}

// a player jumped (a seek, its own or ours): where to
static int seeked(sd_bus_message *msg, void *data, sd_bus_error *error) {
	struct media *m = data;
	struct player *p = by_owner(m, sd_bus_message_get_sender(msg));
	int64_t us;
	if (p && sd_bus_message_read(msg, "x", &us) >= 0) {
		set_position(p, (double)us / 1e6);
		m->changed(m->data);
	}
	return 0;
}

static int listed(sd_bus_message *reply, void *data, sd_bus_error *error) {
	struct media *m = data;
	if (sd_bus_message_is_method_error(reply, NULL)
			|| sd_bus_message_enter_container(reply, 'a', "s") < 0) {
		return 0;
	}
	const char *name;
	while (sd_bus_message_read(reply, "s", &name) > 0) {
		add(m, name, NULL);
	}
	sd_bus_message_exit_container(reply);
	return 0;
}

struct media *media_start(sd_bus *bus, void (*changed)(void *data), void *data) {
	if (!bus) {
		return NULL;
	}
	struct media *m = calloc(1, sizeof(*m));
	if (!m) {
		return NULL;
	}
	m->bus = bus;
	m->changed = changed;
	m->data = data;
	sd_bus_add_match_async(bus, &m->owners, "type='signal',sender='org.freedesktop.DBus',"
		"member='NameOwnerChanged',arg0namespace='org.mpris.MediaPlayer2'", owner_changed, NULL,
		m);
	sd_bus_match_signal_async(bus, &m->properties, NULL, PATH,
		"org.freedesktop.DBus.Properties", "PropertiesChanged", properties_changed, NULL, m);
	sd_bus_match_signal_async(bus, &m->seeked, NULL, PATH, PLAYER_IFACE, "Seeked", seeked, NULL,
		m);
	sd_bus_call_method_async(bus, NULL, "org.freedesktop.DBus", "/org/freedesktop/DBus",
		"org.freedesktop.DBus", "ListNames", listed, m, "");
	return m;
}

void media_stop(struct media *m) {
	if (!m) {
		return;
	}
	sd_bus_slot_unref(m->owners);
	sd_bus_slot_unref(m->properties);
	sd_bus_slot_unref(m->seeked);
	free(m);
}

// the one shown: playing (the latest to change), else the latest to change
static struct player *shown(struct media *m) {
	struct player *best = NULL;
	for (int i = 0; i < PLAYERS; i++) {
		struct player *p = &m->players[i];
		if (!p->used || !p->title[0]) {
			continue;
		}
		if (!best || (p->playing && !best->playing)
				|| (p->playing == best->playing && p->changed_at > best->changed_at)) {
			best = p;
		}
	}
	return best;
}

bool media_get(struct media *m, struct media_now *out) {
	struct player *p = m ? shown(m) : NULL;
	if (!p) {
		return false;
	}
	out->playing = p->playing;
	snprintf(out->title, sizeof(out->title), "%s", p->title);
	snprintf(out->artist, sizeof(out->artist), "%s", p->artist);
	snprintf(out->art, sizeof(out->art), "%s", p->art);
	out->position = position_now(p);
	out->length = p->length;
	out->can_seek = p->can_seek && p->length > 0;
	return true;
}

void media_control(struct media *m, const char *method) {
	struct player *p = m ? shown(m) : NULL;
	if (!p) {
		return;
	}
	sd_bus_call_method_async(m->bus, NULL, p->name, PATH, PLAYER_IFACE, method, NULL, NULL, "");
	sd_bus_flush(m->bus);
}

static int got_position(sd_bus_message *reply, void *data, sd_bus_error *error) {
	struct media *m = data;
	struct player *p = by_owner(m, sd_bus_message_get_sender(reply));
	if (p && !sd_bus_message_is_method_error(reply, NULL)) {
		set_position(p, read_number(reply) / 1e6);
		m->changed(m->data);
	}
	return 0;
}

void media_refresh(struct media *m) {
	struct player *p = m ? shown(m) : NULL;
	if (!p) {
		return;
	}
	sd_bus_call_method_async(m->bus, NULL, p->name, PATH, "org.freedesktop.DBus.Properties",
		"Get", got_position, m, "ss", PLAYER_IFACE, "Position");
	sd_bus_flush(m->bus);
}

void media_seek(struct media *m, double t) {
	struct player *p = m ? shown(m) : NULL;
	if (!p || !p->can_seek) {
		return;
	}
	t = t < 0 ? 0 : p->length > 0 && t > p->length ? p->length : t;
	// to a place in the track it names, or by how far from where it is
	if (p->track[0] == '/') {
		sd_bus_call_method_async(m->bus, NULL, p->name, PATH, PLAYER_IFACE, "SetPosition", NULL,
			NULL, "ox", p->track, (int64_t)(t * 1e6));
	} else {
		sd_bus_call_method_async(m->bus, NULL, p->name, PATH, PLAYER_IFACE, "Seek", NULL, NULL,
			"x", (int64_t)((t - position_now(p)) * 1e6));
	}
	set_position(p, t); // shown there now, the player says so after
	sd_bus_flush(m->bus);
}
