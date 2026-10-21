#include "cursor/dumb.h"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/util/log.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

bool dumb_device_open(struct flame_dumb_device *device, int drm_fd) {
	device->fd = -1;
	if (drm_fd < 0) {
		return false;
	}
	// an empty lease is a new fd on the same device, and DRM master doesn't
	// have to vouch for it
	if (drmIsMaster(drm_fd)) {
		uint32_t lessee;
		int fd = drmModeCreateLease(drm_fd, NULL, 0, O_CLOEXEC, &lessee);
		if (fd >= 0) {
			device->fd = fd;
			return true;
		}
	}
	char *name = drmGetDeviceNameFromFd2(drm_fd);
	if (!name) {
		return false;
	}
	int fd = open(name, O_RDWR | O_CLOEXEC);
	free(name);
	if (fd < 0) {
		return false;
	}
	// a primary node needs master's blessing to touch dumb buffers
	if (drmIsMaster(drm_fd) && drmGetNodeTypeFromFd(fd) == DRM_NODE_PRIMARY) {
		drm_magic_t magic;
		if (drmGetMagic(fd, &magic) < 0 || drmAuthMagic(drm_fd, magic) < 0) {
			close(fd);
			return false;
		}
	}
	device->fd = fd;
	return true;
}

void dumb_device_close(struct flame_dumb_device *device) {
	if (device->fd >= 0) {
		close(device->fd);
		device->fd = -1;
	}
}

static struct flame_dumb_buffer *from_buffer(struct wlr_buffer *buffer);

static void buffer_destroy(struct wlr_buffer *wlr_buffer) {
	struct flame_dumb_buffer *buffer = from_buffer(wlr_buffer);
	// its destroy event and addons (a renderer's import) go first
	wlr_buffer_finish(wlr_buffer);
	if (buffer->dmabuf_fd >= 0) {
		close(buffer->dmabuf_fd);
	}
	if (buffer->data && buffer->data != MAP_FAILED) {
		munmap(buffer->data, buffer->size);
	}
	if (buffer->fd >= 0) {
		drmModeDestroyDumbBuffer(buffer->fd, buffer->handle);
		close(buffer->fd);
	}
	free(buffer);
}

static bool buffer_get_dmabuf(struct wlr_buffer *wlr_buffer,
		struct wlr_dmabuf_attributes *attribs) {
	struct flame_dumb_buffer *buffer = from_buffer(wlr_buffer);
	*attribs = (struct wlr_dmabuf_attributes){
		.width = buffer->base.width,
		.height = buffer->base.height,
		.format = DRM_FORMAT_ARGB8888,
		.modifier = DRM_FORMAT_MOD_LINEAR,
		.n_planes = 1,
		.offset[0] = 0,
		.stride[0] = buffer->stride,
		.fd[0] = buffer->dmabuf_fd,
	};
	return true;
}

static bool buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer, uint32_t flags,
		void **data, uint32_t *format, size_t *stride) {
	struct flame_dumb_buffer *buffer = from_buffer(wlr_buffer);
	*data = buffer->data;
	*format = DRM_FORMAT_ARGB8888;
	*stride = buffer->stride;
	return true;
}

static void buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {
	// mapped for good, nothing to do
}

static const struct wlr_buffer_impl buffer_impl = {
	.destroy = buffer_destroy,
	.get_dmabuf = buffer_get_dmabuf,
	.begin_data_ptr_access = buffer_begin_data_ptr_access,
	.end_data_ptr_access = buffer_end_data_ptr_access,
};

static struct flame_dumb_buffer *from_buffer(struct wlr_buffer *wlr_buffer) {
	struct flame_dumb_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);
	return buffer;
}

struct flame_dumb_buffer *dumb_buffer_create(struct flame_dumb_device *device,
		int width, int height) {
	if (device->fd < 0) {
		return NULL;
	}
	struct flame_dumb_buffer *buffer = calloc(1, sizeof(*buffer));
	if (!buffer) {
		return NULL;
	}
	wlr_buffer_init(&buffer->base, &buffer_impl, width, height);
	buffer->dmabuf_fd = -1;
	buffer->fd = fcntl(device->fd, F_DUPFD_CLOEXEC, 0);
	if (buffer->fd < 0) {
		goto fail;
	}
	if (drmModeCreateDumbBuffer(buffer->fd, width, height, 32, 0, &buffer->handle,
			&buffer->stride, &buffer->size) != 0) {
		wlr_log_errno(WLR_DEBUG, "dumb buffer %dx%d", width, height);
		close(buffer->fd);
		buffer->fd = -1;
		goto fail;
	}
	uint64_t offset;
	if (drmModeMapDumbBuffer(buffer->fd, buffer->handle, &offset) != 0) {
		goto fail;
	}
	buffer->data = mmap(NULL, buffer->size, PROT_READ | PROT_WRITE, MAP_SHARED,
		buffer->fd, (off_t)offset);
	if (buffer->data == MAP_FAILED) {
		goto fail;
	}
	if (drmPrimeHandleToFD(buffer->fd, buffer->handle, DRM_CLOEXEC, &buffer->dmabuf_fd) != 0) {
		goto fail;
	}
	return buffer;
fail:
	wlr_buffer_drop(&buffer->base);
	return NULL;
}
