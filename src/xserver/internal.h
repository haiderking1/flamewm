#ifndef FLAME_XSERVER_INTERNAL_H
#define FLAME_XSERVER_INTERNAL_H

#include "xserver/wire.h"
#include "xserver/xserver.h"

// between the pieces of xserver/, nobody else includes this

enum {
	XS_ERROR_REQUEST = 1,
	XS_ERROR_VALUE = 2,
	XS_ERROR_WINDOW = 3,
	XS_ERROR_PIXMAP = 4,
	XS_ERROR_ATOM = 5,
	XS_ERROR_FONT = 7,
	XS_ERROR_MATCH = 8,
	XS_ERROR_DRAWABLE = 9,
	XS_ERROR_ACCESS = 10,
	XS_ERROR_ALLOC = 11,
	XS_ERROR_GC = 13,
	XS_ERROR_ID_CHOICE = 14,
	XS_ERROR_NAME = 15,
	XS_ERROR_LENGTH = 16,
};

enum {
	XS_EVENT_FOCUS_IN = 9,
	XS_EVENT_FOCUS_OUT = 10,
	XS_EVENT_EXPOSE = 12,
	XS_EVENT_CREATE_NOTIFY = 16,
	XS_EVENT_DESTROY_NOTIFY = 17,
	XS_EVENT_UNMAP_NOTIFY = 18,
	XS_EVENT_MAP_NOTIFY = 19,
	XS_EVENT_REPARENT_NOTIFY = 21,
	XS_EVENT_CONFIGURE_NOTIFY = 22,
	XS_EVENT_PROPERTY_NOTIFY = 28,
	XS_EVENT_SELECTION_CLEAR = 29,
	XS_EVENT_SELECTION_REQUEST = 30,
	XS_EVENT_SELECTION_NOTIFY = 31,
};

#define XS_MASK_EXPOSURE (1u << 15)
#define XS_MASK_STRUCTURE (1u << 17)
#define XS_MASK_SUBSTRUCTURE (1u << 19)
#define XS_MASK_FOCUS (1u << 21)
#define XS_MASK_PROPERTY (1u << 22)

// lifts the 256KB request limit, xlib and xcb ask for it
#define XS_BIG_REQUESTS_OPCODE 128
// just enough to hand out a GL visual, see glx.c
#define XS_GLX_OPCODE 129
#define XS_GLX_FIRST_EVENT 64
#define XS_GLX_FIRST_ERROR 128
// which process a window belongs to, see xres.c
#define XS_XRES_OPCODE 130
// the gpu path, see dri3.c and present.c. Present's events are generic events
#define XS_GE_OPCODE 131
#define XS_XFIXES_OPCODE 132
#define XS_XFIXES_FIRST_EVENT 66
#define XS_XFIXES_FIRST_ERROR 141
#define XS_SYNC_OPCODE 133
#define XS_SYNC_FIRST_EVENT 68
#define XS_SYNC_FIRST_ERROR 142
#define XS_DRI3_OPCODE 134
#define XS_PRESENT_OPCODE 135
// apps drawing with the cpu share their images through it, see shm.c
#define XS_SHM_OPCODE 136
#define XS_SHM_FIRST_EVENT 70
#define XS_SHM_FIRST_ERROR 145
// pixman's compositing for toolkits: pictures, gradients, trapezoids, glyphs
#define XS_RENDER_OPCODE 137
#define XS_RENDER_FIRST_ERROR 146
// flame's outputs: sizes, positions, refresh rates, see randr.c
#define XS_RANDR_OPCODE 138
#define XS_RANDR_FIRST_EVENT 71
#define XS_RANDR_FIRST_ERROR 151
// XInput 2: devices, smooth scrolling, raw motion, see xinput.c. XI1's 17
// event codes and 5 errors are taken even though nothing sends them
#define XS_XI_OPCODE 139
#define XS_XI_FIRST_EVENT 73
#define XS_XI_FIRST_ERROR 155
// the keyboard as XKB tells it, see xkb.c. one event code, one error
#define XS_XKB_OPCODE 140
#define XS_XKB_FIRST_EVENT 90
#define XS_XKB_FIRST_ERROR 160
// more ids when a client ran out, see ids.c
#define XS_XCMISC_OPCODE 141
// the monitors the old way, idle time, screen power, made up input: see
// xinerama.c, screensaver.c, xtest.c
#define XS_XINERAMA_OPCODE 142
#define XS_SCREENSAVER_OPCODE 143
#define XS_SCREENSAVER_FIRST_EVENT 91
#define XS_DPMS_OPCODE 144
#define XS_XTEST_OPCODE 145
// windows that aren't rectangles, see shape.c
#define XS_SHAPE_OPCODE 146
#define XS_SHAPE_FIRST_EVENT 92

// ---- talking back (xserver.c) ----

void xs_client_send(struct xs_client *client, const void *data, size_t len);
// the next fd that came with the client's requests, -1 when there's none.
// the caller owns it
int xs_client_take_fd(struct xs_client *client);
// fd goes out with the next reply (call before sending it), owned by the client
// from then on. false: too many queued, it's closed
bool xs_client_send_fd(struct xs_client *client, int fd);
void xs_error(struct xs_client *client, uint8_t code, uint32_t value, uint8_t major,
	uint16_t minor);
// a reply under construction: the 32 byte header is in, append the rest,
// then xs_reply_send fixes up the length and sends it
void xs_reply_begin(struct xs_client *client, struct xout *out, uint8_t data);
void xs_reply_send(struct xs_client *client, struct xout *out);
// an event, 32 bytes in host order fields. the sequence number gets stamped
// per recipient
struct xs_event {
	uint8_t bytes[32];
};
void xs_event_send(struct xs_client *client, const struct xs_event *event);
// to everyone who selected one of mask on window
void xs_event_deliver(struct flame_xserver *xserver, struct xs_window *window, uint32_t mask,
	const struct xs_event *event);

// input events: like xs_event_deliver, but a client that selected the XI2
// event evtype on window gets that instead and is skipped here
// repeat_release: an autorepeat's release, clients that asked for detectable
// autorepeat don't get it. clients that never set up XKB don't see the
// layout in the state
void xs_event_deliver_input(struct flame_xserver *xserver, struct xs_window *window,
	uint32_t mask, int evtype, const struct xs_event *event, bool repeat_release);
// anyone xs_event_deliver_input would send it to
bool xs_event_wanted_input(struct xs_window *window, uint32_t mask, int evtype);

// something xbridge cares about changed, it hears once per loop iteration
void xs_changed(struct flame_xserver *xserver);
uint32_t xs_now(struct flame_xserver *xserver);

// event field builders, host byte order, swapped per client on the way out
void xs_event_put8(struct xs_event *event, size_t at, uint8_t v);
void xs_event_put16(struct xs_event *event, size_t at, uint16_t v);
void xs_event_put32(struct xs_event *event, size_t at, uint32_t v);

// ---- the window tree (window.c) ----

struct xs_window *xs_window_create(struct flame_xserver *xserver, struct xs_client *owner,
	uint32_t id, struct xs_window *parent);
void xs_window_destroy(struct flame_xserver *xserver, struct xs_window *window);
void xs_window_map(struct flame_xserver *xserver, struct xs_window *window);
void xs_window_unmap(struct flame_xserver *xserver, struct xs_window *window);
// bits of mask as in ConfigureWindow: 1 x, 2 y, 4 width, 8 height, 16 border,
// 32 sibling, 64 stack mode
void xs_window_configure(struct flame_xserver *xserver, struct xs_window *window,
	uint16_t mask, int x, int y, int width, int height, int border, uint32_t sibling,
	uint8_t stack);
void xs_window_reparent(struct flame_xserver *xserver, struct xs_window *window,
	struct xs_window *parent, int x, int y);
// where its inside starts, root coords
void xs_window_origin(const struct xs_window *window, int *x, int *y);
// the topmost viewable child of window at a root point, NULL for none
struct xs_window *xs_window_child_at(struct xs_window *window, int x, int y);

uint32_t xs_window_mask(const struct xs_window *window, const struct xs_client *client);
void xs_window_set_mask(struct xs_window *window, struct xs_client *client, uint32_t mask);
uint32_t xs_window_all_masks(const struct xs_window *window);

struct xs_property *xs_window_property(struct xs_window *window, uint32_t name);
// mode: 0 replace, 1 prepend, 2 append. false on a format mismatch or no memory
bool xs_window_change_property(struct flame_xserver *xserver, struct xs_window *window,
	uint8_t mode, uint32_t name, uint32_t type, uint8_t format, uint32_t units,
	const uint8_t *data, uint8_t *error);
void xs_window_delete_property(struct flame_xserver *xserver, struct xs_window *window,
	uint32_t name);

// focus moved, FocusOut and FocusIn go out
void xs_focus_set(struct flame_xserver *xserver, uint32_t focus, uint8_t revert);
// the focused window went away or out of sight: where focus goes instead
void xs_focus_lost(struct flame_xserver *xserver, struct xs_window *window);

// everything a client made goes when it does
void xs_client_resources_free(struct flame_xserver *xserver, struct xs_client *client);

// ---- drawing with the cpu (draw.c) ----

// GCs, pixmaps and the core drawing requests. false: not one of those
bool xs_draw_request(struct xs_client *client, uint8_t opcode, const struct xin *request);
// bytes a row of width pixels takes at depth, rows padded to 32 bits
size_t xs_image_stride(int depth, int width);
// a pixman image for depth (1, 8, 24, 32), zeroed, or over bits with stride
pixman_image_t *xs_image_create(int depth, int width, int height, void *bits, size_t stride);
struct xs_pixmap *xs_pixmap_find(struct flame_xserver *xserver, uint32_t id);
// the top-level a window is in, NULL for the root
struct xs_window *xs_window_top(struct xs_window *window);
bool xs_draw_create_pixmap(struct xs_client *client, uint32_t id, int depth, int width,
	int height, void *bits, size_t stride, struct xs_shm_segment *shm, uint8_t major,
	uint16_t minor);
// see PutImage: format 0 XYBitmap, 1 XYPixmap, 2 ZPixmap
void xs_draw_put_image(struct xs_client *client, uint32_t drawable, uint32_t gc,
	uint8_t format, uint8_t depth, int width, int height, int left_pad, int src_x,
	int src_y, int w, int h, int dst_x, int dst_y, const uint8_t *data, size_t stride,
	size_t len, uint8_t major, uint16_t minor);
// ZPixmap rows of a drawable's pixels. false: the error went out
bool xs_draw_get_image(struct xs_client *client, uint32_t drawable, int x, int y, int w, int h,
	uint8_t *out, size_t stride, uint8_t *depth, uint32_t *visual, uint8_t major,
	uint16_t minor);
void xs_gc_destroy(struct xs_gc *gc);
// the window tree changed what shows: backings, backgrounds and Expose
void xs_draw_window_shown(struct flame_xserver *xserver, struct xs_window *window);
void xs_draw_window_hidden(struct flame_xserver *xserver, struct xs_window *window);
void xs_draw_window_moved(struct flame_xserver *xserver, struct xs_window *window, int old_x,
	int old_y, int old_w, int old_h);
void xs_draw_window_freed(struct xs_window *window);
// a viewable window's pixels: its top-level's backing, where the window is in
// it, what of it shows (backing coords, clip must be initialized). false:
// nothing to draw on
bool xs_window_pixels(struct xs_window *window, bool inferiors, pixman_image_t **backing,
	int *dx, int *dy, pixman_region32_t *clip);
// pixels of top's backing changed (backing coords): flame hears about it
void xs_damage(struct flame_xserver *xserver, struct xs_window *top,
	const pixman_region32_t *region);

// ---- RENDER (render.c) ----

void xs_render_request(struct xs_client *client, const struct xin *request);
void xs_render_client_gone(struct flame_xserver *xserver, struct xs_client *client);
void xs_render_finish(struct flame_xserver *xserver);

// ---- RandR (randr.c) ----

void xs_randr_request(struct xs_client *client, const struct xin *request);
// flame's outputs, or one standing for the root while there are none
const struct xs_monitor *xs_monitors(struct flame_xserver *xserver, size_t *len);
void xs_randr_client_gone(struct flame_xserver *xserver, struct xs_client *client);
void xs_randr_window_gone(struct flame_xserver *xserver, uint32_t window);
// the monitors changed: listeners hear
void xs_randr_changed(struct flame_xserver *xserver);

// ---- XInput 2 (xinput.c) ----

enum {
	XS_XI_KEY_PRESS = 2,
	XS_XI_KEY_RELEASE = 3,
	XS_XI_BUTTON_PRESS = 4,
	XS_XI_BUTTON_RELEASE = 5,
	XS_XI_MOTION = 6,
	XS_XI_ENTER = 7,
	XS_XI_LEAVE = 8,
	XS_XI_FOCUS_IN = 9,
	XS_XI_FOCUS_OUT = 10,
	XS_XI_RAW_KEY_PRESS = 13,
	XS_XI_RAW_KEY_RELEASE = 14,
	XS_XI_RAW_BUTTON_PRESS = 15,
	XS_XI_RAW_BUTTON_RELEASE = 16,
	XS_XI_RAW_MOTION = 17,
};

#define XS_XI_KEY_REPEAT (1u << 16)
#define XS_XI_POINTER_EMULATED (1u << 16)

// a key, button or motion event the way XI2 tells it
struct xs_input {
	int evtype;
	uint32_t detail; // keycode, button
	uint32_t time;
	uint16_t state; // core: modifiers, buttons, group
	uint32_t flags; // XS_XI_KEY_REPEAT, XS_XI_POINTER_EMULATED
	uint8_t scroll; // the scroll valuators that moved: 1 vertical, 2 horizontal
	bool core_only; // XI2 has no such event (an autorepeat's release)
};

void xs_xi_request(struct xs_client *client, const struct xin *request);
void xs_xi_client_gone(struct flame_xserver *xserver, struct xs_client *client);
void xs_xi_window_gone(struct flame_xserver *xserver, uint32_t window);
// anyone, or client, selected evtype on window
bool xs_xi_selected(struct flame_xserver *xserver, struct xs_window *window, int evtype);
bool xs_xi_client_selected(struct xs_client *client, struct xs_window *window, int evtype);
// to everyone selecting it on receiver. source: the deepest window, where it
// happened
void xs_xi_device_event(struct flame_xserver *xserver, struct xs_window *receiver,
	struct xs_window *source, const struct xs_input *input);
// XI_Enter and XI_Leave, detail as in core crossing events
void xs_xi_crossing(struct flame_xserver *xserver, int evtype, struct xs_window *window,
	uint8_t detail, uint16_t state, uint32_t time);
void xs_xi_focus(struct flame_xserver *xserver, int evtype, struct xs_window *window);
// raw events go to whoever selected them on the root. motion: accelerated
// and raw deltas
void xs_xi_raw(struct flame_xserver *xserver, int evtype, uint32_t detail, double dx,
	double dy, double raw_dx, double raw_dy, uint32_t time);

// ---- XKB (xkb.c) ----

// a default keymap until flame gives its own. false: no memory, no keymap
bool xs_xkb_init(struct flame_xserver *xserver);
void xs_xkb_finish(struct flame_xserver *xserver);
void xs_xkb_request(struct xs_client *client, const struct xin *request);
// the core keyboard mapping, from the same keymap: keysyms per keycode, and
// a keycode's keysyms (width of them)
#define XS_XKB_CORE_MAX 32 // keysyms per keycode, at most
int xs_xkb_core_width(struct flame_xserver *xserver);
void xs_xkb_core_syms(struct flame_xserver *xserver, uint8_t keycode, uint32_t *out,
	int width);
// the real modifiers a key sets or locks
uint8_t xs_xkb_modmap(struct flame_xserver *xserver, uint8_t keycode);
// the keycodes there are, min returned
uint8_t xs_xkb_range(struct flame_xserver *xserver, uint8_t *max);

// ---- selections (selection.c) ----

void xs_set_selection_owner(struct xs_client *client, const struct xin *request);
void xs_get_selection_owner(struct xs_client *client, const struct xin *request);
void xs_convert_selection(struct xs_client *client, const struct xin *request);
// XFIXES SelectSelectionInput
void xs_selection_select_input(struct xs_client *client, const struct xin *request);
// an event or a property for one of flame's windows: a selection's answer
void xs_selection_window_event(struct flame_xserver *xserver, struct xs_window *window,
	const struct xs_event *event);
void xs_selection_window_property(struct flame_xserver *xserver, struct xs_window *window,
	uint32_t name);
void xs_selection_window_gone(struct flame_xserver *xserver, struct xs_window *window);
// NULL: everyone
void xs_selection_client_gone(struct flame_xserver *xserver, struct xs_client *client);
void xs_selection_finish(struct flame_xserver *xserver);

// ---- flame's own windows (flamewin.c) ----

// SendEvent to one of flame's windows, to whoever made it: a selection's
// answer, or a ClientMessage flame hears through its hook
void xs_flame_window_event(struct flame_xserver *xserver, struct xs_window *window,
	const struct xs_event *event);

// ---- resource ids (ids.c) ----

// a resource of any kind, any client, has it
bool xs_id_in_use(struct flame_xserver *xserver, uint32_t id);
// from the client's range and free: what a create needs
bool xs_id_available(struct xs_client *client, uint32_t id);
void xs_xcmisc_request(struct xs_client *client, const struct xin *request);
// RENDER's pictures and glyph sets
bool xs_render_id_in_use(struct flame_xserver *xserver, uint32_t id);
size_t xs_render_ids(struct flame_xserver *xserver, uint32_t base, uint32_t *out, size_t max);

// ---- the small ones (xinerama.c, screensaver.c, xtest.c) ----

void xs_xinerama_request(struct xs_client *client, const struct xin *request);
void xs_screensaver_request(struct xs_client *client, const struct xin *request);
void xs_dpms_request(struct xs_client *client, const struct xin *request);
void xs_xtest_request(struct xs_client *client, const struct xin *request);

// ---- SHAPE (shape.c) ----

void xs_shape_request(struct xs_client *client, const struct xin *request);
// a point, window coords, inside its bounding and input shapes
bool xs_shape_contains(const struct xs_window *window, int x, int y);
void xs_shape_window_gone(struct flame_xserver *xserver, struct xs_window *window);
void xs_shape_client_gone(struct flame_xserver *xserver, struct xs_client *client);

// ---- MIT-SHM (shm.c) ----

void xs_shm_request(struct xs_client *client, const struct xin *request);
// a pixmap or the id let go of a segment, the last one unmaps it
void xs_shm_unref(struct xs_shm_segment *segment);
void xs_shm_detach_all(struct flame_xserver *xserver, struct xs_client *client);

// ---- requests (requests.c) ----

void xs_request(struct xs_client *client, uint8_t opcode, const struct xin *request);

// ---- GLX (glx.c) ----

void xs_glx_request(struct xs_client *client, const struct xin *request);

// ---- X-Resource (xres.c) ----

void xs_xres_request(struct xs_client *client, const struct xin *request);

// ---- the gpu path (xfixes.c, sync.c, dri3.c, present.c) ----

void xs_xfixes_request(struct xs_client *client, const struct xin *request);
void xs_sync_request(struct xs_client *client, const struct xin *request);
void xs_dri3_request(struct xs_client *client, const struct xin *request);
void xs_present_request(struct xs_client *client, const struct xin *request);

struct xshmfence;
struct xs_fence *xs_fence_create(struct xs_client *client, uint32_t id, struct xshmfence *shm,
	bool triggered);
void xs_fence_destroy(struct xs_fence *fence);
void xs_fence_trigger(struct flame_xserver *xserver, uint32_t id);
void xs_syncobj_destroy(struct flame_xserver *xserver, struct xs_syncobj *syncobj);
void xs_pixmap_destroy(struct flame_xserver *xserver, struct xs_pixmap *pixmap);
// a window's size changed, Present's ConfigureNotify goes out
void xs_present_configure(struct flame_xserver *xserver, struct xs_window *window);
void xs_present_window_gone(struct flame_xserver *xserver, uint32_t window);

#endif
