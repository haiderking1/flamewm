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
// core drawing requests are accepted and dropped: apps that draw with the
// cpu through X (no MIT-SHM, no XRender yet) show nothing. xbridge/ ties
// these windows to flame's, steam's webhelper draws over wayland and runs
// its window logic here

struct flame_xserver;
struct xs_client;

// server side resources, clients get their own ranges above
#define XS_ROOT 0x100
#define XS_COLORMAP 0x101
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

struct xs_selection {
	struct wl_list link; // flame_xserver.selections
	uint32_t atom;
	uint32_t owner; // window
	struct xs_client *client;
	uint32_t time;
};

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
	// the keyboard: keysyms for keycode (evdev + 8), by level. returns how
	// many, at most max
	int (*keysyms)(void *data, uint8_t keycode, uint32_t *out, int max);
	// apps drew into a top-level's backing with the cpu, region in backing
	// coords. at most once per loop iteration and top-level
	void (*damage)(void *data, struct xs_window *top, const pixman_region32_t *region);
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

	struct wl_list clients; // xs_client.link
	struct wl_list windows; // xs_window.all_link
	struct wl_list pixmaps; // xs_pixmap.link
	struct wl_list fences; // xs_fence.link
	struct wl_list syncobjs; // xs_syncobj.link
	struct wl_list regions; // xs_region.link
	struct wl_list present_selects; // xs_present_select.link
	struct wl_list selections; // xs_selection.link
	struct wl_list gcs; // xs_gc.link
	struct wl_list shm_segments; // xs_shm_segment.link
	// RENDER, see render.c: pictures, glyph set ids, and pixman's glyph cache
	struct wl_list pictures, glyphset_ids;
	pixman_glyph_cache_t *glyph_cache;
	struct wl_event_source *damage_idle;
	struct xs_window *root;
	struct xs_atoms atoms;
	uint32_t focus;
	uint8_t focus_revert;
	// the pointer as X windows see it
	int pointer_x, pointer_y;
	uint32_t pointer_window; // the deepest window it's in, 0 for none
	uint32_t grab; // the window a held button grabbed, 0 for none
	uint64_t slots[4]; // client id ranges in use
	uint32_t timestamp; // ms, for events that carry one
};

bool xserver_init(struct flame_xserver *xserver, struct wl_event_loop *loop,
	struct xs_hooks hooks, int width, int height);
// picks a free display number and starts listening. false: no X for anyone,
// flame runs fine without it
bool xserver_start(struct flame_xserver *xserver);
void xserver_finish(struct flame_xserver *xserver);

// the screen got bigger or smaller
void xserver_set_screen_size(struct flame_xserver *xserver, int width, int height);

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
// to the X focus window. keycode is evdev + 8
void xserver_key(struct flame_xserver *xserver, uint8_t keycode, bool pressed, uint16_t state,
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
