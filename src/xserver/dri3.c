#include <drm_fourcc.h>
#include <stdlib.h>
#include <unistd.h>
#include <X11/xshmfence.h>

#include "xserver/internal.h"

// DRI3: X11 apps allocate their own gpu buffers (a render node from here, gbm
// or vulkan on their side) and share them as dmabufs. Present then shows one
// of those in a window, and flame puts it on screen without a copy. 1.4 adds
// syncobjs, the explicit sync nvidia needs to not show half drawn frames

#define DRI3_MINOR 4

static uint32_t format_of(uint8_t depth, uint8_t bpp) {
	switch (depth << 8 | bpp) {
	case 24 << 8 | 32:
		return DRM_FORMAT_XRGB8888;
	case 32 << 8 | 32:
		return DRM_FORMAT_ARGB8888;
	case 30 << 8 | 32:
		return DRM_FORMAT_XRGB2101010;
	case 16 << 8 | 16:
		return DRM_FORMAT_RGB565;
	default:
		return DRM_FORMAT_INVALID;
	}
}

static void close_fds(int *fds, int count) {
	for (int i = 0; i < count; i++) {
		if (fds[i] >= 0) {
			close(fds[i]);
		}
	}
}

// a pixmap around buffers the client sent, the fds are the pixmap's now
static void pixmap_from_buffers(struct xs_client *client, uint8_t minor, uint32_t id,
		int planes, int *fds, uint16_t width, uint16_t height, const uint32_t *strides,
		const uint32_t *offsets, uint8_t depth, uint8_t bpp, uint64_t modifier) {
	uint32_t format = format_of(depth, bpp);
	if (format == DRM_FORMAT_INVALID || !width || !height) {
		close_fds(fds, planes);
		xs_error(client, XS_ERROR_MATCH, id, XS_DRI3_OPCODE, minor);
		return;
	}
	struct xs_pixmap *pixmap = calloc(1, sizeof(*pixmap));
	if (!pixmap) {
		close_fds(fds, planes);
		xs_error(client, XS_ERROR_ALLOC, id, XS_DRI3_OPCODE, minor);
		return;
	}
	pixmap->id = id;
	pixmap->owner = client;
	pixmap->width = width;
	pixmap->height = height;
	pixmap->depth = depth;
	pixmap->bpp = bpp;
	pixmap->planes = planes;
	for (int i = 0; i < planes; i++) {
		pixmap->fds[i] = fds[i];
		pixmap->strides[i] = strides[i];
		pixmap->offsets[i] = offsets[i];
	}
	pixmap->modifier = modifier;
	pixmap->format = format;
	wl_list_insert(&client->xserver->pixmaps, &pixmap->link);
}

static void open_device(struct xs_client *client) {
	struct flame_xserver *xserver = client->xserver;
	int fd = xserver->hooks.open_device ? xserver->hooks.open_device(xserver->hooks.data) : -1;
	if (fd < 0) {
		xs_error(client, XS_ERROR_MATCH, 0, XS_DRI3_OPCODE, 1);
		return;
	}
	xs_client_send_fd(client, fd);
	struct xout out;
	xs_reply_begin(client, &out, 1); // one fd
	xs_reply_send(client, &out);
}

static void supported_modifiers(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	uint32_t format = format_of(xin8(in, 8), xin8(in, 9));
	uint64_t modifiers[64];
	size_t count = 0;
	if (format != DRM_FORMAT_INVALID && xserver->hooks.modifiers) {
		count = xserver->hooks.modifiers(xserver->hooks.data, format, modifiers, 64);
	}
	struct xout out;
	xs_reply_begin(client, &out, 0);
	// what a window can flip and what the screen can show: the same here
	xout32(&out, (uint32_t)count);
	xout32(&out, (uint32_t)count);
	xout_zero(&out, 16);
	for (int list = 0; list < 2; list++) {
		for (size_t i = 0; i < count; i++) {
			xout64(&out, modifiers[i]);
		}
	}
	xs_reply_send(client, &out);
}

static struct xs_pixmap *find_pixmap(struct flame_xserver *xserver, uint32_t id) {
	struct xs_pixmap *pixmap;
	wl_list_for_each(pixmap, &xserver->pixmaps, link) {
		if (pixmap->id == id) {
			return pixmap;
		}
	}
	return NULL;
}

static void buffers_from_pixmap(struct xs_client *client, const struct xin *in, uint8_t minor) {
	struct xs_pixmap *pixmap = find_pixmap(client->xserver, xin32(in, 4));
	if (!pixmap || pixmap->planes == 0) {
		xs_error(client, XS_ERROR_PIXMAP, xin32(in, 4), XS_DRI3_OPCODE, minor);
		return;
	}
	for (int i = 0; i < pixmap->planes; i++) {
		int fd = dup(pixmap->fds[i]);
		if (fd < 0 || !xs_client_send_fd(client, fd)) {
			xs_error(client, XS_ERROR_ALLOC, pixmap->id, XS_DRI3_OPCODE, minor);
			return;
		}
	}
	struct xout out;
	xs_reply_begin(client, &out, (uint8_t)pixmap->planes);
	if (minor == 3) { // BufferFromPixmap, the single plane one
		xout32(&out, pixmap->strides[0] * pixmap->height);
		xout16(&out, pixmap->width);
		xout16(&out, pixmap->height);
		xout16(&out, (uint16_t)pixmap->strides[0]);
		xout8(&out, pixmap->depth);
		xout8(&out, pixmap->bpp);
	} else {
		xout16(&out, pixmap->width);
		xout16(&out, pixmap->height);
		xout32(&out, 0);
		xout64(&out, pixmap->modifier);
		xout8(&out, pixmap->depth);
		xout8(&out, pixmap->bpp);
		xout_zero(&out, 6);
		for (int i = 0; i < pixmap->planes; i++) {
			xout32(&out, pixmap->strides[i]);
		}
		for (int i = 0; i < pixmap->planes; i++) {
			xout32(&out, pixmap->offsets[i]);
		}
	}
	xs_reply_send(client, &out);
}

static void import_syncobj(struct xs_client *client, uint32_t id, int fd) {
	struct xs_syncobj *syncobj = calloc(1, sizeof(*syncobj));
	if (!syncobj) {
		close(fd);
		xs_error(client, XS_ERROR_ALLOC, id, XS_DRI3_OPCODE, 10);
		return;
	}
	syncobj->id = id;
	syncobj->owner = client;
	syncobj->fd = fd;
	wl_list_insert(&client->xserver->syncobjs, &syncobj->link);
}

void xs_syncobj_destroy(struct flame_xserver *xserver, struct xs_syncobj *syncobj) {
	if (xserver->hooks.syncobj_destroyed) {
		xserver->hooks.syncobj_destroyed(xserver->hooks.data, syncobj);
	}
	wl_list_remove(&syncobj->link);
	close(syncobj->fd);
	free(syncobj);
}

void xs_dri3_request(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	struct xout out;
	uint8_t minor = xin8(in, 1);
	switch (minor) {
	case 0: { // QueryVersion: 1.4, or whatever the client knows below that
		uint32_t major = xin32(in, 4), client_minor = xin32(in, 8);
		xs_reply_begin(client, &out, 0);
		xout32(&out, 1);
		xout32(&out, major > 1 || client_minor > DRI3_MINOR ? DRI3_MINOR : client_minor);
		xs_reply_send(client, &out);
		break;
	}
	case 1:
		open_device(client);
		break;
	case 2: { // PixmapFromBuffer: one plane, the driver's own layout
		int fd = xs_client_take_fd(client);
		if (fd < 0) {
			xs_error(client, XS_ERROR_LENGTH, 0, XS_DRI3_OPCODE, minor);
			break;
		}
		uint32_t stride = xin16(in, 20), offset = 0;
		pixmap_from_buffers(client, minor, xin32(in, 4), 1, &fd, xin16(in, 16), xin16(in, 18),
			&stride, &offset, xin8(in, 22), xin8(in, 23), DRM_FORMAT_MOD_INVALID);
		break;
	}
	case 3: // BufferFromPixmap
	case 8: // BuffersFromPixmap
		buffers_from_pixmap(client, in, minor);
		break;
	case 4: { // FenceFromFD: an xshmfence the client and flame share
		int fd = xs_client_take_fd(client);
		struct xshmfence *shm = fd >= 0 ? xshmfence_map_shm(fd) : NULL;
		if (fd >= 0) {
			close(fd); // mapped, the fd isn't needed anymore
		}
		if (!shm || !xs_fence_create(client, xin32(in, 8), shm, xin8(in, 12))) {
			if (shm) {
				xshmfence_unmap_shm(shm);
			}
			xs_error(client, XS_ERROR_ALLOC, xin32(in, 8), XS_DRI3_OPCODE, minor);
		}
		break;
	}
	case 6:
		supported_modifiers(client, in);
		break;
	case 7: { // PixmapFromBuffers: up to four planes, with a modifier
		int planes = xin8(in, 12);
		if (planes < 1 || planes > 4) {
			xs_error(client, XS_ERROR_VALUE, (uint32_t)planes, XS_DRI3_OPCODE, minor);
			break;
		}
		int fds[4] = {-1, -1, -1, -1};
		uint32_t strides[4], offsets[4];
		bool missing = false;
		for (int i = 0; i < planes; i++) {
			fds[i] = xs_client_take_fd(client);
			missing |= fds[i] < 0;
			strides[i] = xin32(in, 20 + (size_t)i * 8);
			offsets[i] = xin32(in, 24 + (size_t)i * 8);
		}
		if (missing) {
			close_fds(fds, planes);
			xs_error(client, XS_ERROR_LENGTH, 0, XS_DRI3_OPCODE, minor);
			break;
		}
		pixmap_from_buffers(client, minor, xin32(in, 4), planes, fds, xin16(in, 16),
			xin16(in, 18), strides, offsets, xin8(in, 52), xin8(in, 53), xin64(in, 56));
		break;
	}
	case 9: // SetDRMDeviceInUse: there's one gpu here
		break;
	case 10: { // ImportSyncobj
		int fd = xs_client_take_fd(client);
		if (fd < 0) {
			xs_error(client, XS_ERROR_LENGTH, 0, XS_DRI3_OPCODE, minor);
			break;
		}
		import_syncobj(client, xin32(in, 4), fd);
		break;
	}
	case 11: { // FreeSyncobj
		struct xs_syncobj *syncobj, *tmp;
		wl_list_for_each_safe(syncobj, tmp, &xserver->syncobjs, link) {
			if (syncobj->id == xin32(in, 4)) {
				xs_syncobj_destroy(xserver, syncobj);
			}
		}
		break;
	}
	default: // FDFromFence among them: the fd isn't kept around
		xs_error(client, XS_ERROR_REQUEST, 0, XS_DRI3_OPCODE, minor);
		break;
	}
}
