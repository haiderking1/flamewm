#ifndef FLAMEBAR_NOTIFY_H
#define FLAMEBAR_NOTIFY_H

#include <cairo.h>
#include <stdbool.h>
#include <stdint.h>
#include <systemd/sd-bus.h>
#include <time.h>

// notifications apps send (org.freedesktop.Notifications on the session
// bus), kept newest first for the bar: a card for a while, then in its
// list till they're dismissed. changed called when one comes, changes or
// goes. another daemon holding the name already: the bar waits its turn

#define NOTE_ACTIONS 4

struct note_action {
	char key[64], label[64];
};

struct note {
	uint32_t id;
	char app[64]; // the app's name, as it says
	char icon[512]; // the app's icon: a name in the icon theme or a file
	char image[512]; // a picture with it (a name or a file), "" for none
	cairo_surface_t *picture; // or one sent in the message, NULL for none
	char summary[256], body[1024];
	struct note_action actions[NOTE_ACTIONS]; // the ones to show, not "default"
	int actions_len;
	bool clickable; // it has a "default" action: what a click on it does
	bool critical; // its card stays till it's seen to
	bool transient; // not kept in the list once its card's gone
	bool resident; // an action picked doesn't close it
	bool timed; // the app said how long it lasts: closed when that's up
	time_t when;
	// its card: up, and till when (seconds, CLOCK_MONOTONIC: fui's time),
	// 0 for till it's closed. the bar moves it on while it's pointed at
	bool card;
	double until;
	bool closed; // gone (the app knows), the bar still taking it away
	// the bar's: how tall it is as a card and in the list, 0 till it's
	// measured (again when it changes)
	double heights[2];
};

struct notify;

struct notify *notify_start(sd_bus *bus, void (*changed)(void *data), void *data);
void notify_stop(struct notify *notify);

// how many there are and each, 0 the newest. closed ones too, till
// notify_forget
int notify_count(const struct notify *notify);
struct note *notify_at(struct notify *notify, int i);

// its card's time ran out: still in the list (one the app timed, or
// a transient one, closed)
void notify_card_done(struct notify *notify, uint32_t id);
// the list's been looked at: no cards for what's in it
void notify_seen(struct notify *notify);
// the user closed it (one, or all)
void notify_dismiss(struct notify *notify, uint32_t id);
void notify_clear(struct notify *notify);
// one of its actions picked ("default": it was clicked). closed after
// unless the app keeps it
void notify_invoke(struct notify *notify, uint32_t id, const char *key);
// a closed one the bar's done taking away
void notify_forget(struct notify *notify, uint32_t id);

// do not disturb: new ones go to the list without a card (critical ones
// still get one)
void notify_set_quiet(struct notify *notify, bool quiet);
bool notify_quiet(const struct notify *notify);

#endif
