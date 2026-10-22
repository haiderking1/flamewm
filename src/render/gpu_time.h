#ifndef FLAME_RENDER_GPU_TIME_H
#define FLAME_RENDER_GPU_TIME_H

#include <stdbool.h>
#include <stdint.h>

struct wlr_renderer;

// how long the gpu took on our frames, from GL_TIME_ELAPSED queries around
// each render pass. results are only ever collected once
// they're available, nothing waits on the gpu. wlroots has its own timer but
// it's built on GL timestamps, and nvidia flags those disjoint every frame

#define GPU_TIME_SLOTS 4

struct flame_gpu_time {
	bool usable;
	void *display, *context; // EGLDisplay, EGLContext of the renderer
	uint32_t queries[GPU_TIME_SLOTS];
	bool busy[GPU_TIME_SLOTS];
	int active; // slot measuring right now, -1 for none
};

// false when the renderer can't time itself (not gles2, no timer queries),
// it stays unusable and every other call does nothing
bool gpu_time_init(struct flame_gpu_time *gpu, struct wlr_renderer *renderer);
void gpu_time_finish(struct flame_gpu_time *gpu);

// around everything a frame sends to the gpu. no free slot (the gpu is that
// far behind) just skips measuring this frame
void gpu_time_begin(struct flame_gpu_time *gpu);
void gpu_time_end(struct flame_gpu_time *gpu);

// finished measurements, in ms. returns how many, the longest goes to max
int gpu_time_collect(struct flame_gpu_time *gpu, double *max_ms);

#endif
