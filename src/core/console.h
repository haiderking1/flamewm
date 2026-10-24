#ifndef FLAME_CORE_CONSOLE_H
#define FLAME_CORE_CONSOLE_H

#include <stdbool.h>

// flame started on a text console (greetd hands its sessions the vt, a tty
// login): what it'd print there goes to $XDG_RUNTIME_DIR/flamewm.log (the
// greeter's flamewm-greeter.log) and the console's left black, no cursor.
// the moment before flame's on the screen and after it's gone shows
// nothing, not a wall of logs. in a terminal it all stays where it was

// before anything logs (wlroots picks its colors by where it writes)
void console_quiet(bool greeter);
// flame's done: the cursor back for whoever has the console next (not
// after the greeter: a session's flame is next)
void console_restore(void);

#endif
