#ifndef FLAME_IPC_COMMANDS_H
#define FLAME_IPC_COMMANDS_H

struct flame_ipc_client;

// run one line from a client and answer it. the commands:
//   subscribe              the state now, and again every time it changes
//   state                  the state now, once
//   workspace <1-10>       go there
//   movetoworkspace <1-10> send the focused window there
//   focus next|prev
//   fullscreen             toggle it on the focused window
//   close                  the focused window
//   quit
//   pointer move X Y | pointer button left|middle|right press|release
//                          fake input, only with FLAME_TEST_INPUT=1 set
// answers are {"ok":true} or {"ok":false,"error":"..."}, the state is its own
// {"event":"state",...} line (state.h)
void ipc_command(struct flame_ipc_client *client, char *line);

#endif
