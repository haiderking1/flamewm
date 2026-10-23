#include "notify.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../flameui/flameui.h"

// the daemon side of the notifications spec (1.2): Notify, CloseNotification,
// GetCapabilities, GetServerInformation, the NotificationClosed and
// ActionInvoked signals. a body's markup is read down to its text, a picture
// sent in the message is kept no bigger than the bar shows it

#define PATH "/org/freedesktop/Notifications"
#define NAME "org.freedesktop.Notifications"
#define NOTES 64
#define CARD_SECONDS 5.0
#define PICTURE_MAX 128

enum closed_why { EXPIRED = 1, DISMISSED = 2, CLOSED_BY_APP = 3 };

struct notify {
	sd_bus *bus;
	sd_bus_slot *object, *name;
	void (*changed)(void *data);
	void *data;
	struct note *notes[NOTES]; // newest first
	int len;
	uint32_t next_id;
	bool quiet;
};

static double now_seconds(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

// src into dst, cut where it fits on a whole character
static void copy_text(char *dst, size_t size, const char *src) {
	size_t len = strlen(src);
	if (len >= size) {
		len = size - 1;
		// not halfway through one: back to where it starts
		while (len > 0 && ((unsigned char)src[len] & 0xc0) == 0x80) {
			len--;
		}
	}
	memcpy(dst, src, len);
	dst[len] = '\0';
}

// a body's markup (<b>, <a href>, &amp;...) down to what it says
static void plain(char *dst, size_t size, const char *src) {
	static const char *const entities[][2] = {
		{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"},
		{"&#39;", "'"}, {"&nbsp;", " "},
	};
	char *text = malloc(strlen(src) + 1);
	if (!text) {
		*dst = '\0';
		return;
	}
	size_t n = 0;
	for (const char *s = src; *s;) {
		if (*s == '<') {
			const char *end = strchr(s, '>');
			if (end) {
				// a line break's a line break, any other tag nothing
				if (!strncasecmp(s, "<br", 3)) {
					text[n++] = '\n';
				}
				s = end + 1;
				continue;
			}
		}
		if (*s == '&') {
			bool took = false;
			for (size_t i = 0; i < sizeof(entities) / sizeof(entities[0]); i++) {
				size_t len = strlen(entities[i][0]);
				if (!strncmp(s, entities[i][0], len)) {
					text[n++] = entities[i][1][0];
					s += len;
					took = true;
					break;
				}
			}
			if (took) {
				continue;
			}
		}
		text[n++] = *s++;
	}
	text[n] = '\0';
	// no blank lines at its ends
	char *start = text;
	while (isspace((unsigned char)*start)) {
		start++;
	}
	while (n > 0 && &text[n - 1] >= start && isspace((unsigned char)text[n - 1])) {
		text[--n] = '\0';
	}
	copy_text(dst, size, start);
	free(text);
}

static void free_note(struct note *note) {
	if (note->picture) {
		cairo_surface_destroy(note->picture);
	}
	free(note);
}

static struct note *find(struct notify *n, uint32_t id) {
	for (int i = 0; i < n->len; i++) {
		if (n->notes[i]->id == id) {
			return n->notes[i];
		}
	}
	return NULL;
}

static void closed(struct notify *n, struct note *note, enum closed_why why) {
	if (note->closed) {
		return;
	}
	note->closed = true;
	note->card = false;
	sd_bus_emit_signal(n->bus, PATH, NAME, "NotificationClosed", "uu", note->id, (uint32_t)why);
	n->changed(n->data);
}

// ---- reading a Notify ----

// a hint's value when it's a string, a bool or a number; false for another
// kind (skipped)
static bool hint_string(sd_bus_message *m, char *out, size_t size) {
	char type;
	const char *contents, *s;
	if (sd_bus_message_peek_type(m, &type, &contents) <= 0 || type != 'v' || !contents
			|| strcmp(contents, "s") || sd_bus_message_read(m, "v", "s", &s) < 0) {
		return false;
	}
	copy_text(out, size, s);
	return true;
}

static bool hint_number(sd_bus_message *m, int *out) {
	char type;
	const char *contents;
	if (sd_bus_message_peek_type(m, &type, &contents) <= 0 || type != 'v' || !contents) {
		return false;
	}
	int r = -1;
	if (!strcmp(contents, "y")) {
		uint8_t v;
		r = sd_bus_message_read(m, "v", "y", &v);
		*out = v;
	} else if (!strcmp(contents, "i") || !strcmp(contents, "u")) {
		int32_t v;
		r = sd_bus_message_read(m, "v", contents, &v);
		*out = v;
	} else if (!strcmp(contents, "b")) {
		int v;
		r = sd_bus_message_read(m, "v", "b", &v);
		*out = v;
	}
	return r >= 0;
}

// a picture sent as pixels (rows of rgb or rgba bytes), premultiplied for
// cairo, no bigger than the bar shows it
static cairo_surface_t *picture_of(sd_bus_message *m) {
	int32_t width, height, stride, alpha, bits, channels;
	const uint8_t *data;
	size_t size;
	if (sd_bus_message_enter_container(m, 'v', "(iiibiiay)") <= 0) {
		return NULL;
	}
	cairo_surface_t *out = NULL;
	if (sd_bus_message_enter_container(m, 'r', "iiibiiay") > 0
			&& sd_bus_message_read(m, "iiibii", &width, &height, &stride, &alpha, &bits,
				&channels) >= 0
			&& sd_bus_message_read_array(m, 'y', (const void **)&data, &size) >= 0) {
		sd_bus_message_exit_container(m);
		bool fits = width > 0 && height > 0 && width <= 4096 && height <= 4096 && bits == 8
			&& (channels == 3 || channels == 4) && stride >= width * channels
			&& size >= (size_t)stride * (size_t)(height - 1) + (size_t)(width * channels);
		cairo_surface_t *full = fits
			? cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height) : NULL;
		if (full && !cairo_surface_status(full)) {
			unsigned char *pixels = cairo_image_surface_get_data(full);
			int out_stride = cairo_image_surface_get_stride(full);
			for (int y = 0; y < height; y++) {
				const uint8_t *row = data + (size_t)y * (size_t)stride;
				uint32_t *to = (uint32_t *)(pixels + (size_t)y * (size_t)out_stride);
				for (int x = 0; x < width; x++) {
					const uint8_t *p = row + x * channels;
					uint32_t a = channels == 4 && alpha ? p[3] : 255;
					to[x] = a << 24 | (p[0] * a / 255) << 16 | (p[1] * a / 255) << 8
						| (p[2] * a / 255);
				}
			}
			cairo_surface_mark_dirty(full);
			double s = fmin(1, (double)PICTURE_MAX / fmax(width, height));
			if (s < 1) {
				int w = (int)fmax(round(width * s), 1), h = (int)fmax(round(height * s), 1);
				out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
				cairo_t *cr = cairo_create(out);
				cairo_scale(cr, s, s);
				cairo_set_source_surface(cr, full, 0, 0);
				cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
				cairo_paint(cr);
				cairo_destroy(cr);
				cairo_surface_destroy(full);
			} else {
				out = full;
			}
		} else if (full) {
			cairo_surface_destroy(full);
		}
	}
	sd_bus_message_exit_container(m);
	return out;
}

struct hints {
	int urgency;
	bool transient, resident;
	char image[512], desktop[256];
	cairo_surface_t *picture;
};

static int read_hints(sd_bus_message *m, struct hints *h) {
	int r = sd_bus_message_enter_container(m, 'a', "{sv}");
	if (r < 0) {
		return r;
	}
	while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		const char *key;
		if ((r = sd_bus_message_read(m, "s", &key)) < 0) {
			return r;
		}
		int number;
		bool took = false;
		if (!strcmp(key, "urgency")) {
			took = hint_number(m, &h->urgency);
		} else if (!strcmp(key, "transient")) {
			took = hint_number(m, &number);
			h->transient = took && number;
		} else if (!strcmp(key, "resident")) {
			took = hint_number(m, &number);
			h->resident = took && number;
		} else if (!strcmp(key, "image-path") || !strcmp(key, "image_path")) {
			took = hint_string(m, h->image, sizeof(h->image));
		} else if (!strcmp(key, "desktop-entry")) {
			took = hint_string(m, h->desktop, sizeof(h->desktop));
		} else if (!h->picture && (!strcmp(key, "image-data") || !strcmp(key, "image_data")
				|| !strcmp(key, "icon_data"))) {
			h->picture = picture_of(m);
			took = true;
		}
		if (!took && (r = sd_bus_message_skip(m, "v")) < 0) {
			return r;
		}
		if ((r = sd_bus_message_exit_container(m)) < 0) {
			return r;
		}
	}
	if (r < 0) {
		return r;
	}
	return sd_bus_message_exit_container(m);
}

// the app's icon: the one it sent, else its .desktop file's (by the hint,
// else by its name), else its name as an icon's
static void app_icon(struct note *note, const char *sent, const char *desktop) {
	if (*sent) {
		copy_text(note->icon, sizeof(note->icon), sent);
		return;
	}
	char name[256];
	snprintf(name, sizeof(name), "%s", note->app);
	for (char *c = name; *c; c++) {
		*c = *c == ' ' ? '-' : (char)tolower((unsigned char)*c);
	}
	if ((*desktop && fui_app_icon(desktop, note->icon, sizeof(note->icon)))
			|| fui_app_icon(name, note->icon, sizeof(note->icon))) {
		return;
	}
	copy_text(note->icon, sizeof(note->icon), *desktop ? desktop : name);
}

// the oldest that can go, gone, for a new one: one without a card, closed
// as expired
static bool make_room(struct notify *n) {
	for (int i = n->len - 1; i >= 0; i--) {
		struct note *note = n->notes[i];
		if (!note->card) {
			bool was_closed = note->closed;
			note->closed = true;
			if (!was_closed) {
				sd_bus_emit_signal(n->bus, PATH, NAME, "NotificationClosed", "uu", note->id,
					(uint32_t)EXPIRED);
			}
			free_note(note);
			memmove(&n->notes[i], &n->notes[i + 1], sizeof(n->notes[0]) * (size_t)(n->len - i - 1));
			n->len--;
			return true;
		}
	}
	return false;
}

// fresh kept: in the place of the one it replaces (its card up again),
// else new at the top. NULL out of memory (its picture's let go)
static struct note *keep(struct notify *n, struct note *fresh, uint32_t replaces) {
	// a new version of one there is: in its place, its card up again
	struct note *note = replaces ? find(n, replaces) : NULL;
	if (note && !note->closed) {
		fresh->id = note->id;
		if (note->picture) {
			cairo_surface_destroy(note->picture);
		}
		*note = *fresh;
	} else {
		if (n->len == NOTES && !make_room(n)) {
			// every one a card still: the oldest goes anyway
			n->notes[n->len - 1]->card = false;
			make_room(n);
		}
		note = malloc(sizeof(*note));
		if (!note) {
			if (fresh->picture) {
				cairo_surface_destroy(fresh->picture);
			}
			return NULL;
		}
		*note = *fresh;
		if (replaces && !find(n, replaces)) {
			note->id = replaces; // the number the app's keeping, as the spec has it
		} else {
			do {
				note->id = n->next_id++;
			} while (!note->id || find(n, note->id));
		}
		memmove(&n->notes[1], &n->notes[0], sizeof(n->notes[0]) * (size_t)n->len);
		n->notes[0] = note;
		n->len++;
	}
	return note;
}

static int on_notify(sd_bus_message *m, void *data, sd_bus_error *error) {
	struct notify *n = data;
	const char *app, *icon, *summary, *body;
	uint32_t replaces;
	int r = sd_bus_message_read(m, "susss", &app, &replaces, &icon, &summary, &body);
	if (r < 0) {
		return r;
	}
	struct note fresh = {0};
	copy_text(fresh.app, sizeof(fresh.app), app);
	copy_text(fresh.summary, sizeof(fresh.summary), summary);
	plain(fresh.body, sizeof(fresh.body), body);
	// actions: key, label, key, label...
	if ((r = sd_bus_message_enter_container(m, 'a', "s")) < 0) {
		return r;
	}
	const char *key, *label;
	while ((r = sd_bus_message_read(m, "s", &key)) > 0) {
		if (sd_bus_message_read(m, "s", &label) <= 0) {
			break;
		}
		if (!strcmp(key, "default")) {
			fresh.clickable = true;
		} else if (fresh.actions_len < NOTE_ACTIONS && *label) {
			struct note_action *a = &fresh.actions[fresh.actions_len++];
			copy_text(a->key, sizeof(a->key), key);
			copy_text(a->label, sizeof(a->label), label);
		}
	}
	if ((r = sd_bus_message_exit_container(m)) < 0) {
		return r;
	}
	struct hints hints = {.urgency = 1};
	int32_t timeout;
	if ((r = read_hints(m, &hints)) < 0 || (r = sd_bus_message_read(m, "i", &timeout)) < 0) {
		if (hints.picture) {
			cairo_surface_destroy(hints.picture);
		}
		return r;
	}
	app_icon(&fresh, icon, hints.desktop);
	// its picture, unless that's the icon again (notify-send -i sends both)
	const char *image = hints.image, *same = fresh.icon;
	image += strncmp(image, "file://", 7) ? 0 : 7;
	same += strncmp(same, "file://", 7) ? 0 : 7;
	if (strcmp(image, same)) {
		copy_text(fresh.image, sizeof(fresh.image), image);
	}
	fresh.picture = hints.picture;
	fresh.critical = hints.urgency >= 2;
	fresh.transient = hints.transient;
	fresh.resident = hints.resident;
	fresh.timed = timeout > 0;
	fresh.when = time(NULL);
	fresh.card = !n->quiet || fresh.critical;
	fresh.until = fresh.critical || timeout == 0 ? 0
		: now_seconds() + (timeout > 0 ? fmax(timeout / 1000.0, 2) : CARD_SECONDS);

	struct note *note = keep(n, &fresh, replaces);
	if (!note) {
		return -ENOMEM;
	}
	n->changed(n->data);
	return sd_bus_reply_method_return(m, "u", note->id);
}

static int on_close(sd_bus_message *m, void *data, sd_bus_error *error) {
	struct notify *n = data;
	uint32_t id;
	int r = sd_bus_message_read(m, "u", &id);
	if (r < 0) {
		return r;
	}
	struct note *note = find(n, id);
	if (note) {
		closed(n, note, CLOSED_BY_APP);
	}
	return sd_bus_reply_method_return(m, "");
}

static int on_capabilities(sd_bus_message *m, void *data, sd_bus_error *error) {
	sd_bus_message *reply;
	int r = sd_bus_message_new_method_return(m, &reply);
	if (r < 0) {
		return r;
	}
	r = sd_bus_message_append_strv(reply, (char *[]){"actions", "body", "body-markup",
		"icon-static", "persistence", NULL});
	if (r >= 0) {
		r = sd_bus_send(NULL, reply, NULL);
	}
	sd_bus_message_unref(reply);
	return r;
}

static int on_information(sd_bus_message *m, void *data, sd_bus_error *error) {
	return sd_bus_reply_method_return(m, "ssss", "flame", "flame", "0.1", "1.2");
}

static const sd_bus_vtable vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Notify", "susssasa{sv}i", "u", on_notify, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("CloseNotification", "u", "", on_close, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("GetCapabilities", "", "as", on_capabilities, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("GetServerInformation", "", "ssss", on_information,
		SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_SIGNAL("NotificationClosed", "uu", 0),
	SD_BUS_SIGNAL("ActionInvoked", "us", 0),
	SD_BUS_VTABLE_END,
};

static int named(sd_bus_message *m, void *data, sd_bus_error *error) {
	const sd_bus_error *e = sd_bus_message_get_error(m);
	uint32_t how = 0;
	if (e) {
		fprintf(stderr, "flamebar: no notifications: %s\n", e->message);
	} else if (sd_bus_message_read(m, "u", &how) >= 0 && how == 2) {
		// DBUS_REQUEST_NAME_REPLY_IN_QUEUE
		fprintf(stderr, "flamebar: another app shows notifications, ours once it's gone\n");
	}
	return 0;
}

struct notify *notify_start(sd_bus *bus, void (*changed)(void *data), void *data) {
	if (!bus) {
		return NULL;
	}
	struct notify *n = calloc(1, sizeof(*n));
	if (!n) {
		return NULL;
	}
	n->bus = sd_bus_ref(bus);
	n->changed = changed;
	n->data = data;
	n->next_id = 1;
	if (sd_bus_add_object_vtable(bus, &n->object, PATH, NAME, vtable, n) < 0
			|| sd_bus_request_name_async(bus, &n->name, NAME, SD_BUS_NAME_QUEUE, named, n) < 0) {
		fprintf(stderr, "flamebar: no notifications\n");
		notify_stop(n);
		return NULL;
	}
	return n;
}

void notify_stop(struct notify *n) {
	if (!n) {
		return;
	}
	if (n->name) {
		sd_bus_release_name(n->bus, NAME);
	}
	sd_bus_slot_unref(n->name);
	sd_bus_slot_unref(n->object);
	for (int i = 0; i < n->len; i++) {
		free_note(n->notes[i]);
	}
	sd_bus_unref(n->bus);
	free(n);
}

int notify_count(const struct notify *n) {
	return n ? n->len : 0;
}

struct note *notify_at(struct notify *n, int i) {
	return n && i >= 0 && i < n->len ? n->notes[i] : NULL;
}

void notify_card_done(struct notify *n, uint32_t id) {
	struct note *note = n ? find(n, id) : NULL;
	if (!note || !note->card) {
		return;
	}
	note->card = false;
	if (note->transient || note->timed) {
		closed(n, note, EXPIRED);
	}
	n->changed(n->data);
}

void notify_seen(struct notify *n) {
	for (int i = 0; n && i < n->len; i++) {
		notify_card_done(n, n->notes[i]->id);
	}
}

void notify_dismiss(struct notify *n, uint32_t id) {
	struct note *note = n ? find(n, id) : NULL;
	if (note) {
		closed(n, note, DISMISSED);
	}
}

void notify_clear(struct notify *n) {
	for (int i = 0; n && i < n->len; i++) {
		closed(n, n->notes[i], DISMISSED);
	}
}

void notify_invoke(struct notify *n, uint32_t id, const char *key) {
	struct note *note = n ? find(n, id) : NULL;
	if (!note || note->closed) {
		return;
	}
	sd_bus_emit_signal(n->bus, PATH, NAME, "ActionInvoked", "us", id, key);
	if (!note->resident) {
		closed(n, note, DISMISSED);
	}
}

void notify_forget(struct notify *n, uint32_t id) {
	for (int i = 0; n && i < n->len; i++) {
		if (n->notes[i]->id == id && n->notes[i]->closed) {
			free_note(n->notes[i]);
			memmove(&n->notes[i], &n->notes[i + 1], sizeof(n->notes[0]) * (size_t)(n->len - i - 1));
			n->len--;
			return;
		}
	}
}

void notify_post(struct notify *n, const char *app, const char *icon, const char *summary,
		const char *body, bool critical, uint32_t *id) {
	if (!n) {
		return;
	}
	struct note fresh = {0};
	copy_text(fresh.app, sizeof(fresh.app), app);
	copy_text(fresh.icon, sizeof(fresh.icon), icon);
	copy_text(fresh.summary, sizeof(fresh.summary), summary);
	copy_text(fresh.body, sizeof(fresh.body), body);
	fresh.critical = critical;
	fresh.when = time(NULL);
	fresh.card = !n->quiet || critical;
	fresh.until = critical ? 0 : now_seconds() + CARD_SECONDS;
	struct note *note = keep(n, &fresh, *id);
	if (note) {
		*id = note->id;
		n->changed(n->data);
	}
}

void notify_set_quiet(struct notify *n, bool quiet) {
	if (n && n->quiet != quiet) {
		n->quiet = quiet;
		n->changed(n->data);
	}
}

bool notify_quiet(const struct notify *n) {
	return n && n->quiet;
}
