#ifndef FLAME_BINDINGS_ACTION_H
#define FLAME_BINDINGS_ACTION_H

#include "bindings/keybind.h"

// everything a keybind can do. arg meaning is noted per action
void action_spawn(struct flame_server *server, const union flame_arg *arg); // argv
void action_quit(struct flame_server *server, const union flame_arg *arg);
void action_close(struct flame_server *server, const union flame_arg *arg);
void action_focus(struct flame_server *server, const union flame_arg *arg); // i: +1/-1
void action_fullscreen(struct flame_server *server, const union flame_arg *arg);
void action_workspace(struct flame_server *server, const union flame_arg *arg); // i: index
void action_move_to_workspace(struct flame_server *server, const union flame_arg *arg); // i: index

#endif
