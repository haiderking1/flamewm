#ifndef FLAME_IMAGE_IMAGE_H
#define FLAME_IMAGE_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// picture files read into pixels: jpeg, png and webp, told apart by what's
// in them, not their names. the pixels are cairo's ARGB32 (premultiplied,
// native endian, DRM's ARGB8888 on little endian machines). safe to call
// from any thread

enum image_type {
	IMAGE_NONE, // not a picture flame reads
	IMAGE_JPEG,
	IMAGE_PNG,
	IMAGE_WEBP,
};

struct image {
	int width, height, stride; // stride in bytes
	uint32_t *data; // height rows of stride bytes, malloc'd
};

// what the file at path is by its first bytes, IMAGE_NONE when it isn't one
// (or can't be read)
enum image_type image_sniff(const char *path);

// path read whole. want_width x want_height: the size it's going to be shown
// at, a jpeg comes out as small as it can while still covering that (half,
// a quarter, an eighth of its size, much faster), 0 for its full size. false
// with error filled when it can't be read
bool image_load(const char *path, int want_width, int want_height, struct image *out,
	char *error, size_t error_size);
void image_finish(struct image *image);

#endif
