#ifndef FLAMEBAR_MEDIA_H
#define FLAMEBAR_MEDIA_H

#include <limits.h>
#include <stdbool.h>
#include <systemd/sd-bus.h>

// what's playing, from the players on the session bus (mpris): the one
// playing, else the one that changed last. changed called when any of it
// does

struct media;

struct media_now {
	bool playing;
	char title[256], artist[256];
	char art[PATH_MAX]; // a picture file, "" for none
	// where it is in it, how long it is (seconds, 0: it doesn't say), and
	// whether it can be moved to another place in it
	double position, length;
	bool can_seek;
};

struct media *media_start(sd_bus *bus, void (*changed)(void *data), void *data);
void media_stop(struct media *media);

// false with no player
bool media_get(struct media *media, struct media_now *now);
// to the player shown: "PlayPause", "Next", "Previous"
void media_control(struct media *media, const char *method);
// where the shown one is, asked again (players don't say as they play)
void media_refresh(struct media *media);
// the shown one to t seconds in
void media_seek(struct media *media, double t);

#endif
