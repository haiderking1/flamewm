#ifndef FLAME_IPC_STATE_H
#define FLAME_IPC_STATE_H

struct flame_desktop;
struct strbuf;

// everything a bar shows, as one json line:
// {"event":"state","workspace":1,
//  "workspaces":[{"id":1,"name":"1","windows":2,"active":true,"fullscreen":false},...],
//  "window":{"app_id":"foot","title":"~","fullscreen":false,"floating":false,
//   "x":6,"y":6,"width":1268,"height":708}}  (on screen, border included)
// workspace is the visible one, window the focused one or null
void ipc_state_write(struct flame_desktop *desktop, struct strbuf *out);

#endif
