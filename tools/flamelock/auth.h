#ifndef FLAMELOCK_AUTH_H
#define FLAMELOCK_AUTH_H

#include "../flameui/flameui.h"

// the user's password checked with pam, on a thread of its own: the lock
// keeps drawing while it's at it (a wrong one takes a couple of seconds).
// the password's copied into memory that's never swapped out, and wiped
// once it's checked

enum auth_state { AUTH_IDLE, AUTH_CHECKING, AUTH_OK, AUTH_FAILED };

// ui woken when it's done
void auth_check(struct fui *ui, const char *password);
enum auth_state auth_state(void);
// a failed one seen to: ready for the next
void auth_again(void);

#endif
