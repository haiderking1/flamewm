#ifndef FLAMEPLAY_GPU_H
#define FLAMEPLAY_GPU_H

#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <stdbool.h>
#include <stdint.h>

// decoding on the gpu: ffmpeg decodes with vulkan (video decode queues), our
// pass turns each frame into rgb in images flame can show (dmabufs in a
// layout it imports). the frame never leaves the gpu, the cpu never
// touches a pixel. the decoder's own images can't be shared (the driver
// keeps them in its own layout), so it's one compute pass, like mpv's

struct gpu;

// a picture's image as flame takes it: one plane, a dmabuf
struct gpu_image {
	int fd; // the caller's to close
	uint32_t fourcc;
	uint64_t modifier;
	uint32_t offset, stride;
	int width, height;
};

// a vulkan device that decodes video and shares images, or NULL. *device:
// it for ffmpeg (a reference of the caller's)
struct gpu *gpu_create(AVBufferRef **device);
void gpu_destroy(struct gpu *gpu);

// count rgb images width x height made, their dmabufs in out. false when
// they can't be: decode on the cpu then
bool gpu_images(struct gpu *gpu, int width, int height, int count, struct gpu_image *out);

// frame (decoded on the gpu) into image index, done when this returns. its
// colours by what the frame says it is
bool gpu_convert(struct gpu *gpu, AVFrame *frame, int index);

#endif
