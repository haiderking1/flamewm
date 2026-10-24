#ifndef FLAMEGREET_POWER_H
#define FLAMEGREET_POWER_H

#include <stdbool.h>

// the computer asleep, restarted or off, asked of logind: the login screen
// is the seat's session, that's allowed without a password

enum power { POWER_SLEEP, POWER_RESTART, POWER_OFF, POWER_COUNT };

// what logind says can be done here (a desktop that can't sleep: no sleep)
void power_check(bool can[POWER_COUNT]);
bool power_do(enum power what);

#endif
