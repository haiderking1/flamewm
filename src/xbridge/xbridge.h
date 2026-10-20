#ifndef FLAME_XBRIDGE_XBRIDGE_H
#define FLAME_XBRIDGE_XBRIDGE_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <wayland-server-core.h>
#include <wlr/util/box.h>

#include "xserver/xserver.h"

struct flame_server;
struct wlr_surface;

// apps that run their window logic over X11 but draw over wayland, steam's
// webhelper: valve's code makes, maps, titles and focuses X windows on
// flame's own X server (xserver/), and CEF draws every one of them as a
// wayland toplevel that never hides. this ties the two together: each X
// "view" (a window inside one of the app's top-levels, where CEF lives) is
// one wayland toplevel, matched by creation order within the process. what
// the X side does with it says what flame does with the toplevel:
//
// - unmapped on X: not shown at all
// - in a normal top-level: a tiled window, titled like the X window
// - in an override-redirect top-level: a menu, exactly where X has it
// - a second view in the same top-level: glued inside the first one at its
//   offset (steam's store page under the main window's top bar)
//
// flame moves the X windows to wherever it put them, so the app places its
// menus against where the window really is, and X input focus and flame's
// focus follow each other

enum flame_xbridge_role {
	FLAME_XBRIDGE_HIDDEN,
	FLAME_XBRIDGE_WINDOW,
	FLAME_XBRIDGE_POPUP,
	FLAME_XBRIDGE_EMBED,
};

struct flame_xbridge_verdict {
	enum flame_xbridge_role role;
	// layout coords for popups, inside the host's content for embeds
	struct wlr_box box;
	uint32_t host; // embeds: the view they're inside
	char *title; // NULL for none
	// popups: opened by a click, so they hold the pointer like an X11 grab.
	// hover dropdowns don't, they live off the pointer staying on their tab
	bool grabs;
};

// a process with X connections. claimed: its wayland toplevels are bridged
struct flame_xbridge_process {
	struct wl_list link; // flame_xbridge.processes
	struct flame_xbridge *xbridge;
	pid_t pid;
	char comm[32];
	int connections;
	bool claimed;
	uint32_t created; // toplevels so far, the next one is created + 1
	struct flame_xbridge_verdict *verdicts; // by index - 1
	size_t verdicts_len;
	struct wl_list windows; // flame_xbridge_window.link
	// its X views in creation order, 0 once destroyed. view n is toplevel n
	uint32_t *views;
	size_t views_len, views_cap;
	// the view X input focus is on, 0 for none of them
	uint32_t focus;
	// tells when it exits, its windows aren't bridged from then on
	int pidfd;
	struct wl_event_source *exit_source;
};

// a shell window that may be bridged, embedded in the shell's toplevel
struct flame_xbridge_window {
	struct wl_list link; // flame_xbridge_process.windows
	struct flame_xbridge_process *process; // NULL: not bridged (anymore)
	uint32_t index;
	const void *view; // the flame_view it is, to tell when flame focuses it
	struct wlr_box placed; // last placement handed to the X side
	// the verdict changed or the process is gone, apply it again
	void (*changed)(struct flame_xbridge_window *window);
};

struct flame_xbridge {
	struct flame_server *server;
	struct flame_xserver xserver;
	bool running;
	struct wl_list processes; // flame_xbridge_process.link
	// flame's focused view the last time X focus was told about it
	const void *flame_focus;
	// the pointer's last move was a button, not motion: a popup that shows up
	// now was clicked open
	bool clicked;
	struct wl_listener layout_change;
	struct wl_listener desktop_changed;
};

bool xbridge_init(struct flame_xbridge *xbridge, struct flame_server *server);
// the X server starts listening, $FLAMEWM_X11_DISPLAY says where. flame runs
// fine without it
void xbridge_start(struct flame_xbridge *xbridge);
void xbridge_finish(struct flame_xbridge *xbridge);

// a new toplevel from client: bridged when its process is claimed
void xbridge_window_attach(struct flame_xbridge *xbridge, struct flame_xbridge_window *window,
	struct wl_client *client, const void *view,
	void (*changed)(struct flame_xbridge_window *window));
void xbridge_window_detach(struct flame_xbridge_window *window);
// NULL when it's not bridged, treat it like any window
const struct flame_xbridge_verdict *xbridge_window_verdict(
	const struct flame_xbridge_window *window);
// flame put a bridged window's content here
void xbridge_window_placed(struct flame_xbridge_window *window, struct wlr_box box);
// another window of the same process, NULL when it doesn't exist (yet)
struct flame_xbridge_window *xbridge_window_sibling(struct flame_xbridge_window *window,
	uint32_t index);
// window came or went on screen: what's embedded in it gets applied again
void xbridge_window_embeds_changed(struct flame_xbridge_window *window);
// this window has X input focus
bool xbridge_window_x_focused(const struct flame_xbridge_window *window);
// the window of the same process X input focus is on, NULL for none
struct flame_xbridge_window *xbridge_window_x_focus(struct flame_xbridge_window *window);

// one of surface's app's menus is open and has X focus. on X11 the app holds
// a pointer grab then and its other windows see no motion: no hover, so
// nothing behind the menu (steam's tab dropdowns) pops up and closes it
bool xbridge_menu_holds_pointer(struct flame_xbridge *xbridge, struct wlr_surface *surface);
// its X windows are bookkeeping, not something to show: steam's UI draws over
// wayland, its X side is bridged
bool xbridge_hides(struct flame_xbridge *xbridge, pid_t pid);
// a button went down on surface: like an X11 window manager, X focus goes to
// the window that was clicked. a menu losing it that way is how apps close it
void xbridge_pointer_pressed(struct flame_xbridge *xbridge, struct wlr_surface *surface);
// what the pointer did last: a button (true) or motion (false)
void xbridge_pointer_input(struct flame_xbridge *xbridge, bool button);

#endif
