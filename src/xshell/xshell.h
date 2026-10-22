#ifndef FLAME_XSHELL_XSHELL_H
#define FLAME_XSHELL_XSHELL_H

#include <pixman.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <wayland-server-core.h>

#include "desktop/view.h"
#include "util/timer.h"
#include "xshell/ewmh.h"

struct flame_buffer_node;
struct flame_frame;
struct flame_server;
struct flame_xserver;
struct wlr_drm_syncobj_timeline;
struct wlr_keyboard;
struct flame_tree;
struct xs_window;

// X11 windows as flame windows, like shell/ is for xdg. flame's own X server
// (xserver/) keeps the protocol side, this puts its top-level windows in the
// desktop: normal ones tile like any window, override-redirect ones (menus,
// tooltips) float where the app put them. what they show comes from Present:
// the app's gpu buffer straight into flame's scene, no copy, flame's frame
// clock saying when it reached the screen. a top-level with child windows
// presenting on their own (a browser's web views, a game's client window)
// shows each of them where X has it, clipped and stacked like X would

struct flame_xshell {
	struct flame_server *server;
	struct flame_xserver *xserver;
	struct wl_list windows; // flame_xwindow.link
	struct wl_list frames; // xshell_frame.link, one per output
	struct wl_listener new_output;
	struct wl_listener desktop_changed; // workspaces switched: floats follow
	// the X window the pointer is over, flame's idea of it. 0 for none
	uint32_t pointer_window;
	uint32_t pointer_logged; // the deepest one the log last said it's over
	uint16_t buttons; // held, in X's state bits
	// the key held down that repeats (X keycode, 0 for none) and when it
	// repeats next. X apps get their repeats from the server
	uint8_t repeat_key;
	struct flame_timer repeat_timer;
	// what copies of X apps' presents are done on, see xlayer.copying
	struct wlr_drm_syncobj_timeline *copy_timeline;
	uint64_t copy_point;
	// the X app flame's focus just left (its pid), X's focus comes off it
	// when idle
	int unfocus;
	struct wl_event_source *unfocus_idle;

	// the msc X apps see, Present paces them on it. NVIDIA flips carry no
	// vblank counter, so it's counted here: the clock output's flips move it
	// by however many refreshes passed, between flips it keeps going on the
	// refresh grid. one clock, flame's X has one screen
	struct {
		struct flame_frame *frame; // the output it follows, NULL for none yet
		uint64_t msc;
		int64_t when; // the vblank msc landed on, 0 before it started
		uint64_t seen; // the highest msc handed out, it never goes back
	} clock;
	struct wl_list notifies; // xnotify.link, NotifyMSCs waiting for their msc
	// the next queued present or NotifyMSC comes due
	struct flame_timer msc_timer;

	// X's coordinates are flame's times this: the screen's scale with
	// x_zero_scaling, else 1 (X apps stretched to the screen's scale)
	double scale;
	bool xsettings_on; // xsettings.c's
	uint32_t xsettings_serial;

	// EWMH (ewmh.c): its atoms, the check window, what the root says now
	struct {
		bool started;
		uint32_t atoms[EWMH_ATOMS];
		uint32_t check;
		uint32_t active;
		bool active_set;
		uint32_t clients[512];
		uint32_t clients_len;
	} ewmh;
};

struct flame_xwindow {
	struct wl_list link; // flame_xshell.windows
	struct flame_xshell *xshell;
	uint32_t id; // the X top-level
	bool floating; // override-redirect: placed by the app, no tile
	struct flame_view view; // tiled ones
	struct flame_tree *float_tree; // floating ones
	// its picture, X's window at 0,0: what was drawn with the cpu at the
	// bottom, above that a layer (xlayer) for each window in it that presents
	struct flame_tree *content;
	struct flame_buffer_node *core_node;
	struct wl_list layers; // xlayer.link, bottom to top the way X stacks them
	int width, height; // what X thinks its size is
	// _GTK_FRAME_EXTENTS: its shadow, drawn inside it around the window
	// itself. flame tiles, floats and borders what's inside, X has all of it
	int ext_left, ext_right, ext_top, ext_bottom;
	struct wlr_box placed; // where flame put it last, layout coords
	bool parked; // on a workspace that doesn't show: X has it out of sight

	// drawn with the cpu: its backing as a texture, updated where it changed,
	// and the key telling frames it's a new picture. the top-level's own
	// Present wins over it, its children's go on top of it
	struct xcore *core;
	struct wlr_texture *core_texture;
	uintptr_t core_serial;
	// _NET_WM_STATE as last told, see ewmh.c
	uint32_t ewmh_state;
	bool ewmh_state_set;
};

bool xshell_init(struct flame_xshell *xshell, struct flame_server *server,
	struct flame_xserver *xserver);
void xshell_finish(struct flame_xshell *xshell);

// the X side changed, see which top-levels are on screen
void xshell_windows_changed(struct flame_xshell *xshell);
void xshell_window_destroyed(struct flame_xshell *xshell, struct xs_window *window);
// an X window unmapped (reparenting does too): what it and its children
// showed goes now. an app reusing it elsewhere doesn't flash the old picture
void xshell_window_unmapped(struct flame_xshell *xshell, struct xs_window *window);
// the X server listens: flame says it's the window manager
void xshell_started(struct flame_xshell *xshell);
// flame's layout coords to X's and back, see flame_xshell.scale
int xshell_to_x(const struct flame_xshell *xshell, double v);
double xshell_from_x(const struct flame_xshell *xshell, double v);
// a pointer position to X's: the X pixel it's in (floor), the same one flame
// decides the window under the pointer by
int xshell_pointer_to_x(const struct flame_xshell *xshell, double v);
// an X grab confines the pointer (wine clipping the cursor to a game): the
// layout box it stays in, x1, y1 the furthest it goes
bool xshell_pointer_confine(const struct flame_xshell *xshell, double *x0, double *y0,
	double *x1, double *y1);
// X's cursor changed (a window's, a grab's, hidden): shown if the pointer's X's
void xshell_cursor_changed(struct flame_xshell *xshell);
// the screen's scale or x_zero_scaling changed: X's coordinates follow,
// every X window moves to where it is in them
void xshell_rescale(struct flame_xshell *xshell);
// a client asked to configure a mapped top-level: flame decides, see
// xs_hooks.configure_request. true when it did
bool xshell_configure_request(struct flame_xshell *xshell, struct xs_window *window,
	uint16_t mask, const int32_t values[7]);
// a client message for the window manager (EWMH)
void xshell_wm_message(struct flame_xshell *xshell, uint32_t window, uint32_t type,
	const uint32_t data[5]);

// xserver hooks, xbridge hands them over
struct xs_present;
struct xs_pixmap;
struct xs_syncobj;
struct xs_client;
void xshell_present(struct flame_xshell *xshell, const struct xs_present *present);
void xshell_notify_msc(struct flame_xshell *xshell, uint32_t window, uint32_t serial,
	uint64_t target_msc, uint64_t divisor, uint64_t remainder);
void xshell_pixmap_destroyed(struct flame_xshell *xshell, struct xs_pixmap *pixmap);
void xshell_syncobj_destroyed(struct flame_xshell *xshell, struct xs_syncobj *syncobj);
int xshell_open_device(struct flame_xshell *xshell);
// the GL library X apps should load: "nvidia" on nvidia's driver, else "mesa"
const char *xshell_gl_vendor(struct flame_xshell *xshell);
size_t xshell_modifiers(struct flame_xshell *xshell, uint32_t format, uint64_t *out,
	size_t max);
// the X top-level flame shows at a layout point, 0 when it's something else
uint32_t xshell_window_at(struct flame_xshell *xshell, double lx, double ly);
// the keyboard that typed: its keymap, repeat and modifiers are X's
void xshell_keyboard(struct flame_xshell *xshell, struct wlr_keyboard *keyboard);

// input. true when it went to an X window: the wayland seat stays out of it.
// view is the X window's flame view (for focus) or NULL for a floating one
bool xshell_pointer_motion(struct flame_xshell *xshell, double lx, double ly, uint32_t time,
	struct flame_view **view);
bool xshell_pointer_button(struct flame_xshell *xshell, uint32_t button, bool pressed,
	uint32_t time);
// an X window has the pointer: buttons and the wheel go there
bool xshell_has_pointer(const struct flame_xshell *xshell);
// a button is down on an X window, its implicit grab
bool xshell_buttons_held(const struct flame_xshell *xshell);
// an X client holds the pointer (a menu open): no focus changes meanwhile
bool xshell_pointer_grabbed(const struct flame_xshell *xshell);
// the focused X app has a menu open (a dropdown, a context menu): the
// pointer passing over other windows moves no focus until it closes, like
// a wayland popup's grab. clicks still do
bool xshell_menu_open(struct flame_xshell *xshell);
// the process behind any window, X or wayland (steam's bridged ones), 0
// for unknown
pid_t xshell_view_pid(struct flame_xshell *xshell, struct flame_view *view);
// a key typed in a wayland window: X apps see it the way the config's
// x_global_keys says (push to talk, obs hotkeys, global hotkeys). true: an
// X app's global hotkey took it, the window doesn't get it
bool xshell_key_elsewhere(struct flame_xshell *xshell, struct wlr_keyboard *keyboard,
	uint32_t keycode, bool pressed, uint32_t time);
// a mouse button on a wayland window, as a raw event to X apps that listen
// (x_global_keys = "always")
void xshell_button_elsewhere(struct flame_xshell *xshell, uint32_t button, bool pressed,
	uint32_t time);
bool xshell_pointer_axis(struct flame_xshell *xshell, bool vertical, double delta,
	uint32_t time);
// the mouse moved, before acceleration too: games over X read it raw. only
// while an X window has the pointer or the keyboard
void xshell_pointer_raw_motion(struct flame_xshell *xshell, double dx, double dy,
	double raw_dx, double raw_dy, uint32_t time);
// apps drew into a top-level's backing with the cpu (core requests,
// MIT-SHM): what changed goes up to its texture
void xshell_damaged(struct flame_xshell *xshell, struct xs_window *top,
	const pixman_region32_t *region);

// to the focused X window when flame's focus is on one
bool xshell_key(struct flame_xshell *xshell, struct wlr_keyboard *keyboard, uint32_t keycode,
	bool pressed, uint32_t time);

#endif
