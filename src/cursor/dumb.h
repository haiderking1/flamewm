#ifndef FLAME_CURSOR_DUMB_H
#define FLAME_CURSOR_DUMB_H

#include <stdbool.h>
#include <stdint.h>
#include <wlr/types/wlr_buffer.h>

// cpu-mapped linear buffers KMS can show. nvidia can't render into the linear
// buffers its cursor plane takes, so cursor pixels get written by hand here

// its own DRM fd: GEM handles aren't refcounted, sharing wlroots' fd would
// let it close our handles out from under us (wlroots' own dumb allocator
// does the same)
struct flame_dumb_device {
	int fd;
};

bool dumb_device_open(struct flame_dumb_device *device, int drm_fd);
void dumb_device_close(struct flame_dumb_device *device);

struct flame_dumb_buffer {
	struct wlr_buffer base;
	int fd; // a dup of the device's, so it outlives the device if KMS still has it
	uint32_t handle, stride;
	uint64_t size;
	void *data;
	int dmabuf_fd;
};

// ARGB8888, premultiplied like everything on wayland. drop with wlr_buffer_drop
struct flame_dumb_buffer *dumb_buffer_create(struct flame_dumb_device *device,
	int width, int height);

#endif
