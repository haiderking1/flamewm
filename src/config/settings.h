#ifndef FLAME_CONFIG_SETTINGS_H
#define FLAME_CONFIG_SETTINGS_H

#include <stdbool.h>

struct flame_server;
struct strbuf;

// what flamesettings can change, by key: "gap", "keyboard.layout",
// "output.DP-1.scale". a change goes into settings.lua (one flame.set line a
// key, nothing else in the file touched) and the config reloads, live. what
// flame.lua sets is pinned (config->pinned): it wins, a change to it is an
// error

// the current values, what flame.lua pins and the screens, as one json line
void settings_write(struct flame_server *server, struct strbuf *out);
// value as the key takes it ("8", "true", "#ff8800", "fullscreen", "us,ara").
// false and error says why: no such key, a value that doesn't fit, pinned,
// the file can't be written
bool settings_set(struct flame_server *server, const char *key, const char *value,
	const char **error);
// back to the default (or what flame.lua says)
bool settings_unset(struct flame_server *server, const char *key, const char **error);

#endif
