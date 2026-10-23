#ifndef FLAMESEARCH_USAGE_H
#define FLAMESEARCH_USAGE_H

// what's been opened from the search and how often (an app's id, a file's
// path), kept in $XDG_STATE_HOME/flame/search: what's opened most comes up
// first

void usage_load(void);
void usage_add(const char *key);
// how much higher it goes for being opened before, 0 for never
int usage_boost(const char *key);

#endif
