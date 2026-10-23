#ifndef FLAMEIPC_IPC_H
#define FLAMEIPC_IPC_H

// flame's ipc socket, for flame's apps: $FLAMEWM_SOCKET, else
// $XDG_RUNTIME_DIR/flamewm.$WAYLAND_DISPLAY.sock. one line in, json lines out

// connected, -1 when flame isn't there
int ipc_connect(void);
// one command, its one line answer (free it), NULL when flame isn't there
char *ipc_call(const char *command);

#endif
