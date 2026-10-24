#ifndef FLAMELOCK_LOOK_H
#define FLAMELOCK_LOOK_H

#include <pwd.h>
#include <stdbool.h>
#include <stdint.h>

#include "../flameui/flameui.h"

// how flame asks for a password: the user's picture and name, a field of
// dots under them on glass. the lock and the login screen (flamegreet)
// both look like this

#define LOOK_TEXT 0xffffffff
#define LOOK_TEXT_DIM 0xffffffb3
#define LOOK_FIELD_W 240
#define LOOK_FIELD_H 34
#define LOOK_TYPED_MAX 1024

struct look_person {
	char user[64]; // the login name
	char name[128]; // the full name, else the login name with a capital
	char avatar[4096]; // a picture of them, "" for none
};

void look_person(struct look_person *person, const struct passwd *pw);

// text centered at x, a soft shadow under it
void look_shadowed(struct fui *ui, double x, double y, const char *text, const char *font,
	uint32_t color, double shadow);
// their picture in a circle size across, else the first letter of their
// name: true then, it sits on glass of the caller's
bool look_avatar(struct fui *ui, const struct look_person *person, double cx, double cy,
	double size);

struct look_field {
	int typed_len; // characters, a dot each
	bool checking; // the wheel turns, nothing's typed
	bool awake; // the caret and the ring lit: someone's at it
	double shaken_at; // a wrong one: when it shook
	double typed_at; // the last key: the caret stays lit a moment after
};

// the field at cx, y: where it ended up (it shakes) and how lit, for its
// glass. true: its arrow was clicked
bool look_field(struct fui *ui, const struct look_field *field, double cx, double y, double *at,
	double *lit);

enum look_typed { LOOK_TYPED_NOTHING, LOOK_TYPED_EDITED, LOOK_TYPED_SUBMIT };

// a key into typed (LOOK_TYPED_MAX, *len characters of it): what it did
enum look_typed look_type(char *typed, int *len, uint32_t sym, unsigned mods, const char *text);

#endif
