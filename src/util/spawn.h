#ifndef FLAME_UTIL_SPAWN_H
#define FLAME_UTIL_SPAWN_H

#include <sys/types.h>

// fire and forget, double forks so we never have to reap anything
void spawn(const char *const argv[]);
void spawn_shell(const char *command);
// argv started with what it prints to a pipe, *out its end (non-blocking).
// not double forked: the caller reaps it (a pidfd). -1 when it couldn't be
pid_t spawn_reading(const char *const argv[], int *out);

#endif
