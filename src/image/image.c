#include "image/image.h"

#include <fcntl.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <turbojpeg.h>
#include <unistd.h>
#include <webp/decode.h>

// no bigger than this: 8k is 33M pixels, 64M of them take 256 MiB
#define MAX_SIDE 32768
#define MAX_PIXELS (64 * 1024 * 1024)
#define MAX_FILE (256 * 1024 * 1024)

static enum image_type type_of(const unsigned char *head, size_t len) {
	if (len >= 3 && head[0] == 0xff && head[1] == 0xd8 && head[2] == 0xff) {
		return IMAGE_JPEG;
	}
	if (len >= 8 && !memcmp(head, "\x89PNG\r\n\x1a\n", 8)) {
		return IMAGE_PNG;
	}
	if (len >= 12 && !memcmp(head, "RIFF", 4) && !memcmp(head + 8, "WEBP", 4)) {
		return IMAGE_WEBP;
	}
	return IMAGE_NONE;
}

enum image_type image_sniff(const char *path) {
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return IMAGE_NONE;
	}
	unsigned char head[12];
	ssize_t n = read(fd, head, sizeof(head));
	close(fd);
	return n > 0 ? type_of(head, (size_t)n) : IMAGE_NONE;
}

static void fail(char *error, size_t size, const char *what) {
	if (error && size) {
		snprintf(error, size, "%s", what);
	}
}

// the whole file, NULL when it can't be had
static unsigned char *slurp(const char *path, size_t *len, char *error, size_t error_size) {
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	struct stat st;
	if (fd < 0 || fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
		fail(error, error_size, "can't be opened");
		if (fd >= 0) {
			close(fd);
		}
		return NULL;
	}
	if (st.st_size <= 0 || st.st_size > MAX_FILE) {
		fail(error, error_size, st.st_size ? "too big" : "empty");
		close(fd);
		return NULL;
	}
	unsigned char *data = malloc((size_t)st.st_size);
	size_t have = 0;
	while (data && have < (size_t)st.st_size) {
		ssize_t n = read(fd, data + have, (size_t)st.st_size - have);
		if (n <= 0) {
			break;
		}
		have += (size_t)n;
	}
	close(fd);
	if (!data || have != (size_t)st.st_size) {
		fail(error, error_size, data ? "can't be read" : "out of memory");
		free(data);
		return NULL;
	}
	*len = have;
	return data;
}

static bool sized(struct image *out, int width, int height, char *error, size_t error_size) {
	if (width <= 0 || height <= 0 || width > MAX_SIDE || height > MAX_SIDE
			|| (int64_t)width * height > MAX_PIXELS) {
		fail(error, error_size, "too big");
		return false;
	}
	out->width = width;
	out->height = height;
	out->stride = width * 4;
	out->data = malloc((size_t)out->stride * (size_t)height);
	if (!out->data) {
		fail(error, error_size, "out of memory");
		return false;
	}
	return true;
}

// straight alpha to cairo's premultiplied
static void premultiply(struct image *image) {
	for (int y = 0; y < image->height; y++) {
		uint32_t *row = (uint32_t *)((char *)image->data + (size_t)y * image->stride);
		for (int x = 0; x < image->width; x++) {
			uint32_t p = row[x], a = p >> 24;
			if (a == 255) {
				continue;
			}
			uint32_t r = (p >> 16 & 0xff) * a / 255, g = (p >> 8 & 0xff) * a / 255,
				b = (p & 0xff) * a / 255;
			row[x] = a << 24 | r << 16 | g << 8 | b;
		}
	}
}

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define TJ_FORMAT TJPF_BGRA
#define PNG_FORMAT PNG_FORMAT_BGRA
#else
#define TJ_FORMAT TJPF_ARGB
#define PNG_FORMAT PNG_FORMAT_ARGB
#endif

static bool load_jpeg(const unsigned char *data, size_t len, int want_width, int want_height,
		struct image *out, char *error, size_t error_size) {
	tjhandle tj = tj3Init(TJINIT_DECOMPRESS);
	if (!tj) {
		fail(error, error_size, "out of memory");
		return false;
	}
	bool ok = false;
	if (tj3DecompressHeader(tj, data, len) < 0) {
		fail(error, error_size, tj3GetErrorStr(tj));
		goto out;
	}
	int width = tj3Get(tj, TJPARAM_JPEGWIDTH), height = tj3Get(tj, TJPARAM_JPEGHEIGHT);
	// the smallest it decodes to that still covers what's wanted
	tjscalingfactor best = {1, 1};
	int count = 0;
	const tjscalingfactor *factors = tj3GetScalingFactors(&count);
	for (int i = 0; want_width > 0 && factors && i < count; i++) {
		tjscalingfactor f = factors[i];
		if (f.num >= f.denom || (int64_t)f.num * best.denom >= (int64_t)best.num * f.denom) {
			continue; // no bigger than full size, and smaller than the best so far
		}
		if (TJSCALED(width, f) >= want_width && TJSCALED(height, f) >= want_height) {
			best = f;
		}
	}
	if (tj3SetScalingFactor(tj, best) < 0
			|| !sized(out, TJSCALED(width, best), TJSCALED(height, best), error, error_size)) {
		goto out;
	}
	if (tj3Decompress8(tj, data, len, (unsigned char *)out->data, out->stride, TJ_FORMAT) < 0
			&& tj3GetErrorCode(tj) == TJERR_FATAL) {
		fail(error, error_size, tj3GetErrorStr(tj));
		image_finish(out);
		goto out;
	}
	ok = true; // a warning (a cut off file): what it got is shown
out:
	tj3Destroy(tj);
	return ok;
}

static bool load_png(const unsigned char *data, size_t len, struct image *out, char *error,
		size_t error_size) {
	png_image png = {.version = PNG_IMAGE_VERSION};
	if (!png_image_begin_read_from_memory(&png, data, len)) {
		fail(error, error_size, png.message);
		return false;
	}
	png.format = PNG_FORMAT;
	if (png.width > MAX_SIDE || png.height > MAX_SIDE
			|| !sized(out, (int)png.width, (int)png.height, error, error_size)) {
		fail(error, error_size, "too big");
		png_image_free(&png);
		return false;
	}
	if (!png_image_finish_read(&png, NULL, out->data, out->stride, NULL)) {
		fail(error, error_size, png.message);
		png_image_free(&png);
		image_finish(out);
		return false;
	}
	premultiply(out);
	return true;
}

static bool load_webp(const unsigned char *data, size_t len, struct image *out, char *error,
		size_t error_size) {
	WebPBitstreamFeatures features;
	if (WebPGetFeatures(data, len, &features) != VP8_STATUS_OK) {
		fail(error, error_size, "not a webp it can read");
		return false;
	}
	if (features.has_animation) {
		fail(error, error_size, "an animated webp");
		return false;
	}
	if (!sized(out, features.width, features.height, error, error_size)) {
		return false;
	}
	size_t size = (size_t)out->stride * (size_t)out->height;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	bool decoded = WebPDecodeBGRAInto(data, len, (uint8_t *)out->data, size, out->stride);
#else
	bool decoded = WebPDecodeARGBInto(data, len, (uint8_t *)out->data, size, out->stride);
#endif
	if (!decoded) {
		fail(error, error_size, "a broken webp");
		image_finish(out);
		return false;
	}
	premultiply(out);
	return true;
}

bool image_load(const char *path, int want_width, int want_height, struct image *out,
		char *error, size_t error_size) {
	*out = (struct image){0};
	size_t len;
	unsigned char *data = slurp(path, &len, error, error_size);
	if (!data) {
		return false;
	}
	bool ok = false;
	switch (type_of(data, len)) {
	case IMAGE_JPEG:
		ok = load_jpeg(data, len, want_width, want_height, out, error, error_size);
		break;
	case IMAGE_PNG:
		ok = load_png(data, len, out, error, error_size);
		break;
	case IMAGE_WEBP:
		ok = load_webp(data, len, out, error, error_size);
		break;
	case IMAGE_NONE:
		fail(error, error_size, "not a jpeg, png or webp");
		break;
	}
	free(data);
	return ok;
}

void image_finish(struct image *image) {
	free(image->data);
	*image = (struct image){0};
}
