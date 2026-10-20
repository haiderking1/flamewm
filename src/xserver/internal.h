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
