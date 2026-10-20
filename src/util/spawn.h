#ifndef FLAME_UTIL_SPAWN_H
#define FLAME_UTIL_SPAWN_H

// fire and forget, double forks so we never have to reap anything
void spawn(const char *const argv[]);
void spawn_shell(const char *command);

#endif
