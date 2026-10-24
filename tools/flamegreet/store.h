#ifndef FLAMEGREET_STORE_H
#define FLAMEGREET_STORE_H

#include <stdbool.h>
#include <sys/types.h>

// how the login screen looks while someone's the one picked: their own
// wallpaper, the blur on or off. flamesettings writes it as them, no root:
// /var/lib/flamegreet is everyone's to make their folder in (like /tmp),
// and flamegreet only believes a folder (and what's in it) that's owned by
// whose name it has. FLAMEGREET_DIR for somewhere else (the tests)

struct greet_look {
	char wallpaper[4096]; // a full path, "" for the login screen's own
	bool blur;
};

// whether there's a place for it (flamegreet's installed)
bool store_available(void);
// user's look, uid theirs. anything off about it: the defaults
void store_read(const char *user, uid_t uid, struct greet_look *look);

// the one running this: a copy of the picture at from (NULL: none, the
// login screen's own again), the blur. false and why when it can't
bool store_set_wallpaper(const char *from, const char **error);
bool store_set_blur(bool blur, const char **error);

#endif
