#ifndef FLAMEGREET_PEOPLE_H
#define FLAMEGREET_PEOPLE_H

#include <stdbool.h>

#include "../flamelock/look.h"

// who can log in and into what: the people (not the system's own accounts)
// and the wayland sessions installed, the last ones picked first

#define PEOPLE_MAX 16
#define SESSIONS_MAX 16

struct session {
	char id[128]; // its desktop file's name, no .desktop
	char name[128];
	char exec[1024];
	char desktops[128]; // DesktopNames, ':' between them
};

struct people {
	struct look_person person[PEOPLE_MAX];
	int len;
	struct session session[SESSIONS_MAX];
	int sessions;
	int last_person, last_session; // what was picked last time, else the first
};

void people_load(struct people *people);
// remembered for next time
void people_picked(const struct people *people, int person, int session);

#endif
