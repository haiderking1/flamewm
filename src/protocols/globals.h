#ifndef FLAME_PROTOCOLS_GLOBALS_H
#define FLAME_PROTOCOLS_GLOBALS_H

#include <stdbool.h>

struct flame_server;

// globals wlroots runs on its own once created. anything we have to react
// to (xdg-shell, seat, cursor-shape...) lives in the module that reacts
bool globals_init(struct flame_server *server);

#endif
