#ifndef FLAME_XSHELL_EWMH_H
#define FLAME_XSHELL_EWMH_H

#include <stdint.h>

struct flame_xshell;
struct flame_xwindow;
struct xs_window;

// EWMH: flame as X's window manager, the way apps expect one. a check window
// says one runs, the root says what it supports, which window is active and
// which ones it manages, every window what state it's in (fullscreen,
// focused, on another workspace, tiled shows as maximized like sway does
// it). apps ask through client messages to the root: fullscreen, focus,
// close, their frame extents. and flame owns _NET_WM_CM_S0: something
// composites, so translucent windows are
//
// properties only change when what they say does: every change wakes the
// apps listening

enum ewmh_atom {
	NET_SUPPORTED,
	NET_SUPPORTING_WM_CHECK,
	NET_WM_NAME,
	UTF8_STRING,
	NET_ACTIVE_WINDOW,
	NET_CLIENT_LIST,
	NET_CLOSE_WINDOW,
	NET_WM_STATE,
	NET_WM_STATE_FULLSCREEN,
	NET_WM_STATE_FOCUSED,
	NET_WM_STATE_MAXIMIZED_VERT,
	NET_WM_STATE_MAXIMIZED_HORZ,
	NET_FRAME_EXTENTS,
	NET_REQUEST_FRAME_EXTENTS,
	NET_WM_WINDOW_TYPE,
	NET_WM_MOVERESIZE,
	GTK_FRAME_EXTENTS,
	NET_WM_CM_S0,
	EWMH_ATOMS,
};

// the X server is up: the check window, what's supported, the compositor
// selection
void ewmh_start(struct flame_xshell *xshell);
// a window about to be managed: what it asked for before mapping
// (fullscreen) goes on its view
void ewmh_window_new(struct flame_xshell *xshell, struct flame_xwindow *window,
	struct xs_window *x);
// not managed anymore (unmapped): withdrawn windows have no state
void ewmh_window_gone(struct flame_xshell *xshell, uint32_t id);
// the desktop or the windows changed: the properties catch up
void ewmh_update(struct flame_xshell *xshell);
// a client message for the window manager, about window
void ewmh_message(struct flame_xshell *xshell, uint32_t window, uint32_t type,
	const uint32_t data[5]);

#endif
