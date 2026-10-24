#ifndef FLAMEGREET_GREETD_H
#define FLAMEGREET_GREETD_H

#include "../flameui/flameui.h"

// logging in through greetd ($GREETD_SOCK): the user and their password
// asked about on a thread of its own (pam takes its time with a wrong
// one), the session they picked started once it's right. greetd starts it
// when the greeter's gone, flamegreet exits then

enum greet_state { GREET_IDLE, GREET_CHECKING, GREET_OK, GREET_FAILED };

struct greet_session {
	const char *const *cmd; // NULL ended
	const char *const *env; // KEY=value, NULL ended
};

// ui woken when it's done. password copied into memory that's never
// swapped out, wiped once it's been answered with
void greet_login(struct fui *ui, const char *user, const char *password,
	const struct greet_session *session);
enum greet_state greet_state(void);
// what greetd or pam had to say about the last try ("" for nothing): why
// it failed, or what it said on the way
const char *greet_said(void);
// a failed one seen to: ready for the next
void greet_again(void);

#endif
