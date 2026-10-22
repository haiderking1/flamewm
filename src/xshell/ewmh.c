#include "xshell/ewmh.h"

#include <stdlib.h>
#include <string.h>
#include <linux/input-event-codes.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>

#include "core/server.h"
#include "xserver/xserver.h"
#include "xshell/xshell.h"

static const char *const atom_names[EWMH_ATOMS] = {
	[NET_SUPPORTED] = "_NET_SUPPORTED",
	[NET_SUPPORTING_WM_CHECK] = "_NET_SUPPORTING_WM_CHECK",
	[NET_WM_NAME] = "_NET_WM_NAME",
	[UTF8_STRING] = "UTF8_STRING",
	[NET_ACTIVE_WINDOW] = "_NET_ACTIVE_WINDOW",
	[NET_CLIENT_LIST] = "_NET_CLIENT_LIST",
	[NET_CLOSE_WINDOW] = "_NET_CLOSE_WINDOW",
	[NET_WM_STATE] = "_NET_WM_STATE",
	[NET_WM_STATE_FULLSCREEN] = "_NET_WM_STATE_FULLSCREEN",
	[NET_WM_STATE_FOCUSED] = "_NET_WM_STATE_FOCUSED",
	[NET_WM_STATE_MAXIMIZED_VERT] = "_NET_WM_STATE_MAXIMIZED_VERT",
	[NET_WM_STATE_MAXIMIZED_HORZ] = "_NET_WM_STATE_MAXIMIZED_HORZ",
	[NET_FRAME_EXTENTS] = "_NET_FRAME_EXTENTS",
	[NET_REQUEST_FRAME_EXTENTS] = "_NET_REQUEST_FRAME_EXTENTS",
	[NET_WM_WINDOW_TYPE] = "_NET_WM_WINDOW_TYPE",
	[NET_WM_MOVERESIZE] = "_NET_WM_MOVERESIZE",
	[GTK_FRAME_EXTENTS] = "_GTK_FRAME_EXTENTS",
	[NET_WM_CM_S0] = "_NET_WM_CM_S0",
};

// window types apps set that flame acts on (x_wants_float), told as known
static const char *const window_types[] = {
	"_NET_WM_WINDOW_TYPE_NORMAL", "_NET_WM_WINDOW_TYPE_DIALOG",
	"_NET_WM_WINDOW_TYPE_UTILITY", "_NET_WM_WINDOW_TYPE_SPLASH",
	"_NET_WM_WINDOW_TYPE_TOOLBAR", "_NET_WM_WINDOW_TYPE_MENU",
	"_NET_WM_WINDOW_TYPE_POPUP_MENU", "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU",
	"_NET_WM_WINDOW_TYPE_TOOLTIP", "_NET_WM_WINDOW_TYPE_NOTIFICATION",
};

// state bits as kept per window, in the order they go in _NET_WM_STATE
enum {
	STATE_FULLSCREEN = 1 << 0,
	STATE_FOCUSED = 1 << 1,
	STATE_MAXIMIZED = 1 << 2,
};

static uint32_t atom(struct flame_xshell *xshell, enum ewmh_atom which) {
	return xshell->ewmh.atoms[which];
}

static struct xs_window *root_of(struct flame_xshell *xshell) {
	return xshell->xserver->root;
}

static void set_cardinals(struct flame_xshell *xshell, struct xs_window *x, uint32_t name,
		uint32_t type, const uint32_t *values, uint32_t count) {
	xserver_set_property(xshell->xserver, x, name, type, 32, values, count);
}

void ewmh_start(struct flame_xshell *xshell) {
	struct flame_xserver *xserver = xshell->xserver;
	for (int i = 0; i < EWMH_ATOMS; i++) {
		xshell->ewmh.atoms[i] = xserver_atom(xserver, atom_names[i]);
	}
	uint32_t a_window = xserver_atom(xserver, "WINDOW");
	uint32_t a_atom = xserver_atom(xserver, "ATOM");

	// the check window: it and the root point at it, it has the WM's name
	struct xs_window *check = xserver_flame_window_create(xserver);
	if (!check) {
		wlr_log(WLR_ERROR, "ewmh: no check window, X apps won't see a window manager");
		return;
	}
	xshell->ewmh.check = check->id;
	set_cardinals(xshell, check, atom(xshell, NET_SUPPORTING_WM_CHECK), a_window, &check->id,
		1);
	xserver_set_property(xserver, check, atom(xshell, NET_WM_NAME), atom(xshell, UTF8_STRING),
		8, "flame", 5);
	set_cardinals(xshell, root_of(xshell), atom(xshell, NET_SUPPORTING_WM_CHECK), a_window,
		&check->id, 1);

	uint32_t supported[EWMH_ATOMS + sizeof(window_types) / sizeof(window_types[0])];
	uint32_t n = 0;
	for (int i = 0; i < EWMH_ATOMS; i++) {
		if (i != UTF8_STRING && i != NET_WM_CM_S0) {
			supported[n++] = atom(xshell, (enum ewmh_atom)i);
		}
	}
	for (size_t i = 0; i < sizeof(window_types) / sizeof(window_types[0]); i++) {
		supported[n++] = xserver_atom(xserver, window_types[i]);
	}
	set_cardinals(xshell, root_of(xshell), atom(xshell, NET_SUPPORTED), a_atom, supported, n);

	// flame composites: apps may use translucent windows
	xserver_selection_own(xserver, atom(xshell, NET_WM_CM_S0), true);
	xshell->ewmh.started = true;
}

// ICCCM's WM_STATE: the window manager's word that it has the window, state
// and icon window. Normal while flame manages it (a hidden workspace too, it
// isn't iconic), Withdrawn once the app unmaps it. wine waits for it before
// it lets its window take focus: without it a game never became foreground,
// keys and mouse look went nowhere. set the moment flame takes the window
// on, before focusing it: wine drops a WM_TAKE_FOCUS that comes first
#define WM_STATE_WITHDRAWN 0
#define WM_STATE_NORMAL 1

static void set_wm_state(struct flame_xshell *xshell, struct xs_window *x, uint32_t state) {
	uint32_t wm_state = xserver_atom(xshell->xserver, "WM_STATE");
	const uint32_t value[2] = {state, 0}; // no icon window
	set_cardinals(xshell, x, wm_state, wm_state, value, 2);
}

void ewmh_window_new(struct flame_xshell *xshell, struct flame_xwindow *window,
		struct xs_window *x) {
	if (!xshell->ewmh.started) {
		return;
	}
	if (!x->override_redirect) {
		set_wm_state(xshell, x, WM_STATE_NORMAL); // managed, popups never are
	}
	// no decorations from flame on the X side: its borders aren't X's
	static const uint32_t none[4] = {0, 0, 0, 0};
	set_cardinals(xshell, x, atom(xshell, NET_FRAME_EXTENTS),
		xserver_atom(xshell->xserver, "CARDINAL"), none, 4);
	// what it asked for before mapping: games and players start fullscreen
	struct xs_property *state = xserver_property(xshell->xserver, x, "_NET_WM_STATE");
	if (!state || state->format != 32 || window->floating) {
		return;
	}
	for (uint32_t i = 0; i < state->units; i++) {
		uint32_t a;
		memcpy(&a, state->data + i * 4, 4);
		if (a == atom(xshell, NET_WM_STATE_FULLSCREEN)) {
			window->view.fullscreen_wanted = true;
		}
	}
}

void ewmh_window_gone(struct flame_xshell *xshell, uint32_t id) {
	struct xs_window *x = xserver_window(xshell->xserver, id);
	if (xshell->ewmh.started && x) {
		xserver_delete_property(xshell->xserver, x, atom(xshell, NET_WM_STATE));
		set_wm_state(xshell, x, WM_STATE_WITHDRAWN);
	}
}

static uint32_t state_of(struct flame_xshell *xshell, struct flame_xwindow *window) {
	struct flame_desktop *desktop = &xshell->server->desktop;
	struct flame_view *view = &window->view;
	uint32_t bits = 0;
	if (view->fullscreen_wanted) {
		bits |= STATE_FULLSCREEN;
	} else if (!view->floating) {
		bits |= STATE_MAXIMIZED; // a tile, square and shadowless like sway's
	}
	if (desktop->focused == view) {
		bits |= STATE_FOCUSED;
	}
	// another workspace isn't _NET_WM_STATE_HIDDEN: that's minimized, and
	// chromium (steam, discord, browsers) stops drawing and drops its
	// frames on it, back on the workspace it shows white till it redraws
	return bits;
}

static void write_state(struct flame_xshell *xshell, struct xs_window *x, uint32_t bits) {
	uint32_t atoms[4];
	uint32_t n = 0;
	if (bits & STATE_FULLSCREEN) {
		atoms[n++] = atom(xshell, NET_WM_STATE_FULLSCREEN);
	}
	if (bits & STATE_FOCUSED) {
		atoms[n++] = atom(xshell, NET_WM_STATE_FOCUSED);
	}
	if (bits & STATE_MAXIMIZED) {
		atoms[n++] = atom(xshell, NET_WM_STATE_MAXIMIZED_VERT);
		atoms[n++] = atom(xshell, NET_WM_STATE_MAXIMIZED_HORZ);
	}
	set_cardinals(xshell, x, atom(xshell, NET_WM_STATE), xserver_atom(xshell->xserver, "ATOM"),
		atoms, n);
}

void ewmh_update(struct flame_xshell *xshell) {
	if (!xshell->ewmh.started) {
		return;
	}
	struct flame_xserver *xserver = xshell->xserver;
	struct flame_desktop *desktop = &xshell->server->desktop;
	uint32_t list[512];
	uint32_t n = 0;
	uint32_t active = 0;
	struct flame_xwindow *window;
	wl_list_for_each(window, &xshell->windows, link) {
		struct xs_window *x = window->floating ? NULL : xserver_window(xserver, window->id);
		if (!x) {
			continue;
		}
		if (n < sizeof(list) / sizeof(list[0])) {
			list[n++] = window->id;
		}
		if (desktop->focused == &window->view) {
			active = window->id;
		}
		uint32_t bits = state_of(xshell, window);
		if (!window->ewmh_state_set || bits != window->ewmh_state) {
			window->ewmh_state = bits;
			window->ewmh_state_set = true;
			write_state(xshell, x, bits);
		}
	}
	struct xs_window *root = root_of(xshell);
	if (!xshell->ewmh.active_set || active != xshell->ewmh.active) {
		xshell->ewmh.active = active;
		xshell->ewmh.active_set = true;
		set_cardinals(xshell, root, atom(xshell, NET_ACTIVE_WINDOW),
			xserver_atom(xserver, "WINDOW"), &active, 1);
	}
	if (n != xshell->ewmh.clients_len
			|| memcmp(list, xshell->ewmh.clients, n * sizeof(list[0]))) {
		memcpy(xshell->ewmh.clients, list, n * sizeof(list[0]));
		xshell->ewmh.clients_len = n;
		set_cardinals(xshell, root, atom(xshell, NET_CLIENT_LIST),
			xserver_atom(xserver, "WINDOW"), list, n);
	}
}

static struct flame_xwindow *managed(struct flame_xshell *xshell, uint32_t id) {
	struct flame_xwindow *window;
	wl_list_for_each(window, &xshell->windows, link) {
		if (window->id == id && !window->floating) {
			return window;
		}
	}
	return NULL;
}

// _NET_WM_MOVERESIZE's directions, top-left clockwise, then move
static const uint32_t direction_edges[8] = {
	WLR_EDGE_TOP | WLR_EDGE_LEFT, WLR_EDGE_TOP, WLR_EDGE_TOP | WLR_EDGE_RIGHT, WLR_EDGE_RIGHT,
	WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT, WLR_EDGE_BOTTOM, WLR_EDGE_BOTTOM | WLR_EDGE_LEFT,
	WLR_EDGE_LEFT,
};
#define DIRECTION_MOVE 8
#define DIRECTION_CANCEL 11

// a headerbar or a decoration's edge dragged: flame's drag takes it from
// there. the app saw the button go down and lets go of the pointer: its
// release is the drag's, X forgets it's held
static void move_resize(struct flame_xshell *xshell, struct flame_xwindow *window,
		const uint32_t data[5]) {
	struct flame_drag *drag = &xshell->server->input.drag;
	uint32_t direction = data[2];
	if (direction == DIRECTION_CANCEL) {
		drag_forget(drag, &window->view);
		return;
	}
	if (direction > DIRECTION_MOVE) {
		return; // keyboard moves and sizes: flame's are mod + mouse
	}
	// the button it says, or the one held
	static const uint32_t buttons[] = {0, BTN_LEFT, BTN_MIDDLE, BTN_RIGHT};
	uint32_t xbutton = data[3] < 4 ? data[3] : 0;
	if (!xbutton) {
		for (uint32_t b = 1; b <= 3 && !xbutton; b++) {
			xbutton = xshell->buttons & (1u << (7 + b)) ? b : 0;
		}
	}
	uint32_t button = buttons[xbutton];
	bool move = direction == DIRECTION_MOVE;
	if (drag_request(drag, &window->view, move ? FLAME_DRAG_MOVE : FLAME_DRAG_RESIZE,
			move ? 0 : direction_edges[direction], button) && xbutton) {
		uint16_t bit = (uint16_t)(1u << (7 + xbutton));
		xshell->buttons &= (uint16_t)~bit;
		xshell->xserver->state &= (uint16_t)~bit;
	}
}

#define STATE_REMOVE 0
#define STATE_ADD 1
#define STATE_TOGGLE 2

void ewmh_message(struct flame_xshell *xshell, uint32_t id, uint32_t type,
		const uint32_t data[5]) {
	if (!xshell->ewmh.started) {
		return;
	}
	struct flame_desktop *desktop = &xshell->server->desktop;
	if (type == atom(xshell, NET_REQUEST_FRAME_EXTENTS)) {
		// asked before mapping, so the app can size itself: none
		struct xs_window *x = xserver_window(xshell->xserver, id);
		static const uint32_t none[4] = {0, 0, 0, 0};
		if (x) {
			set_cardinals(xshell, x, atom(xshell, NET_FRAME_EXTENTS),
				xserver_atom(xshell->xserver, "CARDINAL"), none, 4);
		}
		return;
	}
	struct flame_xwindow *window = managed(xshell, id);
	if (!window) {
		return;
	}
	struct flame_view *view = &window->view;
	if (type == atom(xshell, NET_WM_STATE)) {
		uint32_t action = data[0];
		for (int i = 1; i <= 2; i++) {
			if (data[i] != atom(xshell, NET_WM_STATE_FULLSCREEN)) {
				continue; // the rest is flame's to say: tiled, focused, hidden
			}
			bool on = action == STATE_ADD
				|| (action == STATE_TOGGLE && !view->fullscreen_wanted);
			if (action <= STATE_TOGGLE) {
				fullscreen_request(desktop, view, on);
			}
		}
		// what it is now, whatever was asked: an app asking to be maximized
		// hears it still is or isn't
		window->ewmh_state_set = false;
		ewmh_update(xshell);
	} else if (type == atom(xshell, NET_ACTIVE_WINDOW)) {
		// apps asking for focus get it on the workspace that shows, they
		// don't pull the user to another one. and only from their own
		// windows, like windows' foreground lock: another app the user is
		// in keeps it. wine asks as a pager with no timestamp, so neither
		// says anything (a game took focus back from a tool next to it
		// every few seconds, every key went to the game)
		struct flame_view *focused = desktop->focused;
		pid_t pid = xshell_view_pid(xshell, view);
		if (view->workspace != desktop->active) {
			return;
		}
		if (focused && focused != view && (!pid || pid != xshell_view_pid(xshell, focused))) {
			wlr_log(WLR_DEBUG, "xshell: 0x%x asks for the focus, another app has it: refused",
				id);
			return;
		}
		focus_view(desktop, view);
	} else if (type == atom(xshell, NET_CLOSE_WINDOW)) {
		view_close(view);
	} else if (type == atom(xshell, NET_WM_MOVERESIZE)) {
		move_resize(xshell, window, data);
	}
}
