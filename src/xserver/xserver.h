#ifndef FLAME_XSERVER_XSERVER_H
#define FLAME_XSERVER_XSERVER_H

#include <pixman.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <wayland-server-core.h>

#include "util/strbuf.h"
#include "xserver/atoms.h"

// flame's own X11 server, instead of xwayland. it keeps the window tree,
// properties, atoms, selections and focus, answers the core protocol, and
// takes what gpu apps draw with: DRI3 buffers, Present, SYNC fences and
// explicit sync, GLX enough for Mesa to pick a driver. their pictures go
// straight into flame's scene (xshell/), no second compositor in between.
// apps drawing with the cpu (core requests, MIT-SHM, RENDER) draw into a
// backing per top-level that flame shows as a texture. RandR tells them
// about flame's outputs. xbridge/ ties these windows to flame's

struct flame_xserver;
struct xs_client;
struct xs_xkb;
struct xkb_keymap;

// server side resources, clients get their own ranges above
#define XS_ROOT 0x100
#define XS_COLORMAP 0x101
#define XS_FLAME_WINDOW 0x102 // flame's own, owns the selections it bridges
#define XS_VISUAL_24 0x21
#define XS_VISUAL_32 0x22 // argb, for windows with depth 32
#define XS_FOCUS_NONE 0
#define XS_FOCUS_POINTER_ROOT 1

struct xs_property {
	struct wl_list link; // xs_window.properties
	uint32_t name, type;
	uint8_t format; // 8, 16 or 32
	uint32_t units; // in format sized units
	uint8_t *data; // host byte order
};

// what one client wants to hear about one window
struct xs_interest {
	struct wl_list link; // xs_window.interests
	struct xs_client *client;
	uint32_t mask;
};

struct xs_window {
	struct wl_list link; // parent's children, bottom to top
	struct wl_list children;
	struct wl_list all_link; // flame_xserver.windows
	struct xs_window *parent; // NULL for the root
	uint32_t id;
	struct xs_client *owner; // NULL for the root
	int16_t x, y; // relative to the parent, outer corner
	uint16_t width, height, border;
	uint8_t depth;
	uint32_t visual;
	uint16_t class; // 1 InputOutput, 2 InputOnly
	bool override_redirect;
	bool mapped;
	uint32_t colormap;
	uint16_t do_not_propagate;
	struct wl_list properties; // xs_property.link
	struct wl_list interests; // xs_interest.link
	void *data; // xbridge's

	// what ClearArea and exposures paint: XS_BACKGROUND_*
	uint8_t background;
	uint32_t background_pixel, background_pixmap;
	// top-levels apps draw into with the cpu (core requests, MIT-SHM): the
	// pixels of its whole tree, each window at its offset. NULL while
	// unmapped. damage: backing coords, changed since flame last looked
	pixman_image_t *backing;
	pixman_region32_t damage;
	bool damaged;
	// something real was drawn since it mapped: flame shows it from then on
	bool painted;
	// SHAPE: bounding, clip, input. NULL for its plain rectangle
	pixman_region32_t *shapes[3];
};

enum {
	XS_BACKGROUND_NONE,
	XS_BACKGROUND_PIXEL,
	XS_BACKGROUND_PIXMAP, // tiled
	XS_BACKGROUND_PARENT, // ParentRelative
};

// a pixmap: cpu pixels drawing requests go into, or a gpu buffer the
// client shared (DRI3) that Present shows in a window
struct xs_pixmap {
	struct wl_list link; // flame_xserver.pixmaps
	uint32_t id;
	struct xs_client *owner;
	uint16_t width, height;
	uint8_t depth, bpp;
	// DRI3: the dmabuf, planes == 0 for none. owned by the pixmap
	int planes;
	int fds[4];
	uint32_t strides[4], offsets[4];
	uint64_t modifier;
	uint32_t format; // drm fourcc
	void *data; // flame's, its texture of it
	// cpu pixmaps: its pixels, maybe in a shared memory segment it keeps alive
	pixman_image_t *image;
	struct xs_shm_segment *shm;
};

// a graphics context: how drawing requests draw
struct xs_gc {
	struct wl_list link; // flame_xserver.gcs
	uint32_t id;
	struct xs_client *owner;
	uint8_t depth; // of the drawable it was made for
	uint8_t function; // GXcopy and friends
	uint32_t plane_mask, foreground, background;
	uint16_t line_width;
	uint8_t line_style, cap_style, join_style, fill_style, fill_rule, arc_mode;
	uint32_t tile, stipple; // pixmap ids, 0 for none
	int16_t ts_x, ts_y; // tile and stipple origin
	uint32_t font;
	uint8_t subwindow_mode; // 0 ClipByChildren, 1 IncludeInferiors
	bool graphics_exposures;
	int16_t clip_x, clip_y; // clip origin
	// clip: none (everything), rectangles or a bitmap's set pixels, in
	// drawable coords before the clip origin
	bool clipped;
	pixman_region32_t clip;
};

// a MIT-SHM segment the client shares (SysV or an fd), mapped here
struct xs_shm_segment {
	struct wl_list link; // flame_xserver.shm_segments, unlinked when detached
	uint32_t id;
	struct xs_client *owner;
	uint8_t *data;
	size_t size;
	bool read_only, sysv;
	int refs; // its id, and every pixmap made in it
};

// SYNC fences. from DRI3 they're an xshmfence the client shares, triggered
// when flame is done with a buffer
struct xs_fence {
	struct wl_list link; // flame_xserver.fences
	uint32_t id;
	struct xs_client *owner;
	struct xshmfence *shm; // NULL for a plain one
	bool triggered; // plain ones
};

// a DRM syncobj the client shared (DRI3 1.4), explicit sync timelines
struct xs_syncobj {
	struct wl_list link; // flame_xserver.syncobjs
	uint32_t id;
	struct xs_client *owner;
	int fd;
	void *data; // flame's, the imported timeline
};

// XFIXES regions. Present takes them, nothing here looks inside
struct xs_region {
	struct wl_list link; // flame_xserver.regions
	uint32_t id;
	struct xs_client *owner;
	pixman_region32_t region;
};

// who wants Present's events for a window
struct xs_present_select {
	struct wl_list link; // flame_xserver.present_selects
	uint32_t id; // the selection's own XID
	struct xs_client *client;
	uint32_t window;
	uint32_t mask; // 1 configure, 2 complete, 4 idle
};

// a PresentPixmap(Synced), what flame needs to show it
struct xs_present {
	struct xs_client *client;
	uint32_t window;
	struct xs_pixmap *pixmap;
	uint32_t serial;
	int16_t x_off, y_off;
	uint32_t options; // 1 async, 2 copy, 4 ust, 8 suboptimal, 16 async may tear
	uint64_t target_msc, divisor, remainder;
	uint32_t idle_fence; // XID, 0 for none
	// PixmapSynced: wait for acquire_point before reading, signal
	// release_point when done
	struct xs_syncobj *acquire, *release;
	uint64_t acquire_point, release_point;
};

// an output as flame lays it out, what RandR tells apps about
struct xs_monitor {
	char name[32];
	int x, y, width, height; // root coords
	int mm_width, mm_height; // 0 when unknown
	int refresh; // mHz, 0 when unknown
	bool primary;
	// the server's: its RandR output is id, its crtc id + 1, its mode id + 2
	uint32_t id;
};

struct xs_selection {
	struct wl_list link; // flame_xserver.selections
	uint32_t atom;
	uint32_t owner; // window
	struct xs_client *client; // NULL with an owner: flame's
	uint32_t time;
};

// an X client wants a selection flame owns. flame answers with
// xserver_selection_answer, then or later
struct xs_selection_request {
	struct wl_list link; // flame_xserver.selection_requests
	uint32_t selection, target, property, requestor, time;
};

// flame asked an X owner for a selection (xserver_selection_convert): the
// answer, in pieces when it's big. last: that was it. type 0 at the end: it
// failed or was refused
struct xs_transfer;
typedef void (*xs_transfer_fn)(void *data, uint32_t type, const uint8_t *bytes, size_t len,
	bool last);

struct xs_client {
	struct wl_list link; // flame_xserver.clients
	struct flame_xserver *xserver;
	int fd;
	struct wl_event_source *source;
	pid_t pid;
	char comm[32];
	int slot; // its resource ids are (slot + 1) << 21 and up
	uint32_t id_base;
	bool setup_done;
	bool announced; // flame heard about it through hooks.connected
	bool big_requests;
	uint16_t seq; // the last request handled
	struct strbuf in, out;
	// file descriptors that came with requests (DRI3 buffers, fences, shm),
	// in order, taken by the requests that carry them
	int in_fds[32];
	int in_fds_len;
	// going out with the next bytes sent, owned until then
	int out_fds[16];
	int out_fds_len;
	bool dead;
	const char *why; // why it was dropped, for the log
	// XKB: it set the extension up (its events carry the layout), it wants
	// autorepeat without the releases, the XKB events it selected and
	// StateNotify's details
	bool xkb, detectable_repeat;
	uint16_t xkb_events, xkb_state_details;
};

// the keyboard's state as XKB has it, real modifiers
struct xs_kbd_state {
	uint8_t base, latched, locked;
	uint8_t group, locked_group; // effective and locked layout
	int16_t base_group, latched_group;
};

// what flame wants to know and decide, see xbridge/
struct xs_hooks {
	// a new connection: false turns it away with a reason
	bool (*allow)(void *data, pid_t pid, const char *comm);
	void (*connected)(void *data, struct xs_client *client);
	void (*disconnected)(void *data, struct xs_client *client);
	// a window was made, before anyone heard about it
	void (*window_created)(void *data, struct xs_window *window);
	// about to go away
	void (*window_destroyed)(void *data, struct xs_window *window);
	// anything about windows that flame may care about changed: mapping,
	// geometry, parents, names. called at most once per loop iteration
	void (*windows_changed)(void *data);
	// a client moved input focus
	void (*focus_changed)(void *data, uint32_t focus);
	// where the pointer is, root coords
	void (*pointer)(void *data, double *x, double *y);
	// DRI3: a render node for the client to allocate on, -1 for none
	int (*open_device)(void *data);
	// DRI3: the modifiers flame can show format with
	size_t (*modifiers)(void *data, uint32_t format, uint64_t *out, size_t max);
	// Present: show this, see xserver_present_*
	void (*present)(void *data, const struct xs_present *present);
	// Present NotifyMSC: tell the client when msc reaches target
	void (*notify_msc)(void *data, struct xs_client *client, uint32_t window,
		uint32_t serial, uint64_t target_msc, uint64_t divisor, uint64_t remainder);
	void (*pixmap_destroyed)(void *data, struct xs_pixmap *pixmap);
	void (*syncobj_destroyed)(void *data, struct xs_syncobj *syncobj);
	// apps drew into a top-level's backing with the cpu, region in backing
	// coords. at most once per loop iteration and top-level
	void (*damage)(void *data, struct xs_window *top, const pixman_region32_t *region);
	// an X client took a selection (x_owned) or the X owner let go of it
	void (*selection_owner)(void *data, uint32_t selection, bool x_owned);
	// an X client wants a selection flame owns
	void (*selection_request)(void *data, struct xs_selection_request *request);
	// a ClientMessage (format 32) for one of flame's windows: drag and drop
	void (*client_message)(void *data, uint32_t window, uint32_t type,
		const uint32_t message[5]);
	void *data;
};

struct flame_xserver {
	struct wl_event_loop *loop;
	struct xs_hooks hooks;
	int display; // -1 until it listens
	char lock_path[64], socket_path[64];
	int fds[2];
	struct wl_event_source *sources[2];
	struct wl_event_source *changed_idle;
	// flame acting on a change changed things again: told on the next loop
	// iteration, never in the same one
	struct wl_event_source *changed_timer;
	bool notifying, changed_again;

	struct wl_list clients; // xs_client.link
	struct wl_list windows; // xs_window.all_link
	struct wl_list pixmaps; // xs_pixmap.link
	struct wl_list fences; // xs_fence.link
	struct wl_list syncobjs; // xs_syncobj.link
	struct wl_list regions; // xs_region.link
	struct wl_list present_selects; // xs_present_select.link
	struct wl_list selections; // xs_selection.link
	// flame in selections, see selection.c: what it asked owners for, what
	// clients asked it for, who listens for owners changing (XFIXES)
	struct wl_list transfers, selection_requests, selection_selects;
	uint32_t flame_window_ids;
	struct wl_list gcs; // xs_gc.link
	struct wl_list shm_segments; // xs_shm_segment.link
	// RENDER, see render.c: pictures, glyph set ids, and pixman's glyph cache
	struct wl_list pictures, glyphset_ids;
	pixman_glyph_cache_t *glyph_cache;
	struct wl_event_source *damage_idle;
	// RandR, see randr.c: flame's outputs, when they last changed (server
	// time), and who listens for that
	struct xs_monitor *monitors;
	size_t monitors_len;
	uint32_t monitors_time, monitor_ids;
	struct wl_list randr_selects;
	// XInput 2, see xinput.c: who selected what, the scroll valuators (in
	// wayland's axis units), wheel notches building up, and whether XI2
	// clients have seen the valuators since the pointer came in
	struct wl_list xi_selects;
	struct wl_list shape_selects; // shape.c's
	struct wl_list damages; // damage.c's
	// composite.c's: who redirected what, who holds the overlay window
	struct wl_list redirects, overlay_refs;
	struct xs_window *overlay;
	double scroll[2], scroll_notches[2];
	bool scroll_primed;
	// XKB, see xkb.c: flame's keymap worked out for X, the keyboard's state,
	// its repeat rate (per second) and delay (ms)
	struct xs_xkb *xkb;
	struct xs_kbd_state kbd;
	int repeat_rate, repeat_delay;
	struct xs_window *root;
	struct xs_atoms atoms;
	uint32_t focus;
	uint8_t focus_revert;
	// the pointer as X windows see it
	int pointer_x, pointer_y;
	uint32_t pointer_window; // the deepest window it's in, 0 for none
	uint32_t grab; // the window a held button grabbed, 0 for none
	uint16_t state; // modifiers and buttons, as the last input had them
	uint32_t last_input; // ms, any input flame got, X windows' or not
	bool trace; // FLAME_X_TRACE=1: every request in the log
	uint64_t slots[4]; // client id ranges in use
	uint32_t timestamp; // ms, for events that carry one
};

bool xserver_init(struct flame_xserver *xserver, struct wl_event_loop *loop,
	struct xs_hooks hooks, int width, int height);
// picks a free display number and starts listening. false: no X for anyone,
// flame runs fine without it
bool xserver_start(struct flame_xserver *xserver);
void xserver_finish(struct flame_xserver *xserver);

// flame's outputs changed: the root covers them all, RandR clients hear.
// ids are filled in here, monitors with the same name keep theirs
void xserver_set_monitors(struct flame_xserver *xserver, const struct xs_monitor *monitors,
	size_t len);

struct xs_window *xserver_window(struct flame_xserver *xserver, uint32_t id);
// a top-level: a child of the root
bool xs_window_top_level(const struct xs_window *window);
// mapped, and so is everything up to the root
bool xs_window_viewable(const struct xs_window *window);
// a property by its atom's name, NULL when it isn't set
struct xs_property *xserver_property(struct flame_xserver *xserver, struct xs_window *window,
	const char *name);
// its name as the app set it (_NET_WM_NAME, else WM_NAME), NULL for none
const char *xs_window_title(struct flame_xserver *xserver, struct xs_window *window,
	char *buf, size_t size);

// flame takes a selection for the X side (the wayland clipboard has
// something) or lets go of it. requests come through hooks.selection_request
void xserver_selection_own(struct flame_xserver *xserver, uint32_t selection, bool own);
// an X client owns it
bool xserver_selection_owned(struct flame_xserver *xserver, uint32_t selection);
// the answer to a request: data as type in format (8, 16, 32), or type 0 for
// no. the request is gone after
void xserver_selection_answer(struct flame_xserver *xserver,
	struct xs_selection_request *request, uint32_t type, uint8_t format, const void *data,
	size_t len);
// asks the X owner for selection as target. NULL: no X owner, or no memory.
// fn hears the answer, fn isn't called after a cancel
struct xs_transfer *xserver_selection_convert(struct flame_xserver *xserver,
	uint32_t selection, uint32_t target, xs_transfer_fn fn, void *data);
void xserver_transfer_cancel(struct xs_transfer *transfer);
// flame's own windows in the X tree, no client behind them: InputOnly,
// unmapped until shown. show puts it on top of everything, at x, y root coords
struct xs_window *xserver_flame_window_create(struct flame_xserver *xserver);
void xserver_flame_window_destroy(struct flame_xserver *xserver, struct xs_window *window);
void xserver_flame_window_show(struct flame_xserver *xserver, struct xs_window *window,
	int x, int y, int width, int height);
void xserver_flame_window_hide(struct flame_xserver *xserver, struct xs_window *window);
// on top of its siblings, like a window manager raising it
void xserver_raise_window(struct flame_xserver *xserver, struct xs_window *window);
// on any window, as a client would set it. units in format sized units
bool xserver_set_property(struct flame_xserver *xserver, struct xs_window *window,
	uint32_t name, uint32_t type, uint8_t format, const void *data, uint32_t units);
void xserver_delete_property(struct flame_xserver *xserver, struct xs_window *window,
	uint32_t name);
// a ClientMessage, format 32, to whoever made window
void xserver_send_client_message(struct flame_xserver *xserver, uint32_t window,
	uint32_t type, const uint32_t data[5]);
// the top-level X has at a root point (its stacking, not flame's), NULL for none
struct xs_window *xserver_top_level_at(struct flame_xserver *xserver, int x, int y);
void xserver_window_root_position(struct xs_window *window, int *x, int *y);
// an atom by name, made when it doesn't exist. NULL name for unknown ones
uint32_t xserver_atom(struct flame_xserver *xserver, const char *name);
const char *xserver_atom_name(struct flame_xserver *xserver, uint32_t atom);

// like a window manager would: the app finds its window where flame put it
void xserver_move_window(struct flame_xserver *xserver, struct xs_window *window,
	int x, int y, int width, int height);
// flame moved focus, the X side follows. focus is a window id or XS_FOCUS_NONE
void xserver_set_focus(struct flame_xserver *xserver, uint32_t focus);

// input for X windows. top: the X top-level flame has under the pointer, NULL
// when it's over something else. state: X's modifier and button mask
void xserver_pointer_motion(struct flame_xserver *xserver, struct xs_window *top, int x,
	int y, uint16_t state, uint32_t time);
void xserver_pointer_button(struct flame_xserver *xserver, uint8_t button, bool pressed,
	uint16_t state, uint32_t time);
void xserver_pointer_leave(struct flame_xserver *xserver, uint32_t time);
// a wheel or touchpad scrolled over the pointer's X window, delta in
// wayland's axis units (15 a notch)
void xserver_pointer_scroll(struct flame_xserver *xserver, bool vertical, double delta,
	uint16_t state, uint32_t time);
// the mouse moved by this much, accelerated and not: games read it raw
void xserver_pointer_raw_motion(struct flame_xserver *xserver, double dx, double dy,
	double raw_dx, double raw_dy, uint32_t time);
// to the X focus window. keycode is evdev + 8
void xserver_key(struct flame_xserver *xserver, uint8_t keycode, bool pressed, uint16_t state,
	uint32_t time);
// the keymap X apps see, flame's. switching to a copy of the same keymap
// (another keyboard) tells nobody, a different one tells everyone
void xserver_set_keymap(struct flame_xserver *xserver, struct xkb_keymap *keymap);
// the keyboard's modifiers (real ones) and layout changed, XKB clients hear
void xserver_keyboard_state(struct flame_xserver *xserver, uint32_t depressed,
	uint32_t latched, uint32_t locked, uint32_t group);
// the names the keymap was built from, for apps that read them off the root
// (_XKB_RULES_NAMES: layout switchers, setxkbmap)
void xserver_set_keymap_names(struct flame_xserver *xserver, const char *rules,
	const char *model, const char *layout, const char *variant, const char *options);
// rate per second, delay in ms, what XKB tells apps
void xserver_set_repeat(struct flame_xserver *xserver, int rate, int delay);
// someone's at the keyboard or the mouse, whatever they're using: the idle
// time X apps ask about starts over
void xserver_activity(struct flame_xserver *xserver);
// a held key repeats, flame keeps time
void xserver_key_repeat(struct flame_xserver *xserver, uint8_t keycode, uint16_t state,
	uint32_t time);
// WM_DELETE_WINDOW when the app said it handles it, else the client goes.
// true for the polite way
bool xserver_close_window(struct flame_xserver *xserver, struct xs_window *window);

// Present events. mode: 0 copy, 1 flip, 2 skip. ust in microseconds
void xserver_present_complete(struct flame_xserver *xserver, uint32_t window,
	uint32_t serial, bool notify_msc, uint8_t mode, uint64_t ust, uint64_t msc);
// flame is done with the pixmap: its idle fence goes off, the client hears
void xserver_present_idle(struct flame_xserver *xserver, uint32_t window, uint32_t serial,
	uint32_t pixmap, uint32_t idle_fence);

#endif
