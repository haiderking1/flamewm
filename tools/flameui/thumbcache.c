#include <errno.h>
#include <fcntl.h>
#include <libavutil/md5.h>
#include <limits.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "internal.h"

// the freedesktop thumbnail cache (~/.cache/thumbnails): pngs named by the
// md5 of the file's uri, saying which file and when it changed. what's in
// it from other apps is used, what flameui makes goes in for them

// a file's uri the way glib writes it: what the md5 is of
static void uri_of(const char *path, char *out, size_t size) {
	size_t n = (size_t)snprintf(out, size, "file://");
	for (const unsigned char *c = (const unsigned char *)path; *c && n + 4 < size; c++) {
		if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9')
				|| strchr("!$&'()*+,-./:=@_~", *c)) {
			out[n++] = (char)*c;
		} else {
			n += (size_t)snprintf(out + n, size - n, "%%%02X", *c);
		}
	}
	out[n] = '\0';
}

static void cache_dir(char *out, size_t size) {
	const char *xdg = getenv("XDG_CACHE_HOME");
	const char *home = getenv("HOME");
	if (xdg && *xdg == '/') {
		snprintf(out, size, "%s/thumbnails", xdg);
	} else {
		snprintf(out, size, "%s/.cache/thumbnails", home ? home : "/tmp");
	}
}

// where path's thumbnail of this size is kept, false for files that don't
// get one kept (the cache's own)
static bool cache_path(const char *path, int size, char *uri, size_t uri_size, char *out,
		size_t out_size) {
	char dir[PATH_MAX];
	cache_dir(dir, sizeof(dir));
	if (!strncmp(path, dir, strlen(dir))) {
		return false;
	}
	uri_of(path, uri, uri_size);
	uint8_t md5[16];
	av_md5_sum(md5, (const uint8_t *)uri, strlen(uri));
	char hex[33];
	for (int i = 0; i < 16; i++) {
		snprintf(hex + 2 * i, 3, "%02x", md5[i]);
	}
	snprintf(out, out_size, "%s/%s/%s.png", dir, size <= 256 ? "large" : "x-large", hex);
	return true;
}

int fui_cache_size(int want) {
	return want <= 256 ? 256 : 512;
}

// straight rgba, png's, from cairo's premultiplied and back
static void unpremultiply(uint32_t *px, size_t n, uint8_t *rgba) {
	for (size_t i = 0; i < n; i++) {
		uint32_t p = px[i], a = p >> 24;
		uint32_t r = p >> 16 & 0xff, g = p >> 8 & 0xff, b = p & 0xff;
		if (a && a < 255) {
			r = r * 255 / a;
			g = g * 255 / a;
			b = b * 255 / a;
		}
		rgba[4 * i] = (uint8_t)r;
		rgba[4 * i + 1] = (uint8_t)g;
		rgba[4 * i + 2] = (uint8_t)b;
		rgba[4 * i + 3] = (uint8_t)a;
	}
}

bool fui_cache_read(const char *path, time_t mtime, int size, struct image *out) {
	char uri[PATH_MAX * 3 + 8], file[PATH_MAX];
	if (!cache_path(path, size, uri, sizeof(uri), file, sizeof(file))) {
		return false;
	}
	FILE *f = fopen(file, "rbe");
	if (!f) {
		return false;
	}
	png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	png_infop info = png ? png_create_info_struct(png) : NULL;
	// what longjmp comes back to: volatile, or it may not be what was set
	png_bytep *volatile rows = NULL;
	volatile bool ok = false;
	*out = (struct image){0};
	if (!info || setjmp(png_jmpbuf(png))) {
		goto out;
	}
	png_init_io(png, f);
	png_read_info(png, info);
	// it says which file, and that file as it was then: else it's stale
	png_textp text;
	int texts = 0;
	png_get_text(png, info, &text, &texts);
	bool same_uri = false, same_time = false;
	for (int i = 0; i < texts; i++) {
		if (!strcmp(text[i].key, "Thumb::URI")) {
			same_uri = !strcmp(text[i].text, uri);
		} else if (!strcmp(text[i].key, "Thumb::MTime")) {
			same_time = strtoll(text[i].text, NULL, 10) == (long long)mtime;
		}
	}
	if (!same_uri || !same_time) {
		goto out;
	}
	png_uint_32 w = png_get_image_width(png, info), h = png_get_image_height(png, info);
	if (w == 0 || h == 0 || w > 4096 || h > 4096) {
		goto out;
	}
	// whatever it was written as, 8 bit rgba out
	png_set_expand(png);
	png_set_strip_16(png);
	png_set_gray_to_rgb(png);
	png_set_add_alpha(png, 0xff, PNG_FILLER_AFTER);
	png_read_update_info(png, info);
	*out = (struct image){.width = (int)w, .height = (int)h, .stride = (int)w * 4};
	out->data = malloc((size_t)w * h * 4);
	rows = malloc(h * sizeof(*rows));
	if (!out->data || !rows) {
		goto out;
	}
	for (png_uint_32 y = 0; y < h; y++) {
		rows[y] = (png_bytep)out->data + (size_t)y * w * 4;
	}
	png_read_image(png, rows);
	// rgba bytes to cairo's premultiplied argb words
	for (size_t i = 0; i < (size_t)w * h; i++) {
		const uint8_t *p = (const uint8_t *)&out->data[i];
		uint32_t r = p[0], g = p[1], b = p[2], a = p[3];
		out->data[i] = a << 24 | (r * a / 255) << 16 | (g * a / 255) << 8 | b * a / 255;
	}
	ok = true;
out:
	if (!ok) {
		image_finish(out);
	}
	free((void *)rows);
	png_destroy_read_struct(&png, &info, NULL);
	fclose(f);
	return ok;
}

static bool mkdirs(const char *path) {
	char p[PATH_MAX];
	snprintf(p, sizeof(p), "%s", path);
	for (char *s = p + 1; *s; s++) {
		if (*s == '/') {
			*s = '\0';
			if (mkdir(p, 0700) < 0 && errno != EEXIST) {
				return false;
			}
			*s = '/';
		}
	}
	return mkdir(p, 0700) == 0 || errno == EEXIST;
}

void fui_cache_write(const char *path, time_t mtime, int size, const struct image *thumb) {
	char uri[PATH_MAX * 3 + 8], file[PATH_MAX];
	if (!cache_path(path, size, uri, sizeof(uri), file, sizeof(file))) {
		return;
	}
	char dir[PATH_MAX];
	snprintf(dir, sizeof(dir), "%s", file);
	*strrchr(dir, '/') = '\0';
	if (!mkdirs(dir)) {
		return;
	}
	// written aside, moved over: no app ever reads half of one
	char tmp[PATH_MAX + 16];
	snprintf(tmp, sizeof(tmp), "%s.XXXXXX", file);
	int fd = mkstemp(tmp);
	FILE *f = fd >= 0 ? fdopen(fd, "wb") : NULL;
	if (!f) {
		if (fd >= 0) {
			close(fd);
			unlink(tmp);
		}
		return;
	}
	uint8_t *rgba = malloc((size_t)thumb->width * thumb->height * 4);
	png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	png_infop info = png ? png_create_info_struct(png) : NULL;
	volatile bool ok = false;
	if (!rgba || !info || setjmp(png_jmpbuf(png))) {
		goto out;
	}
	for (int y = 0; y < thumb->height; y++) {
		unpremultiply((uint32_t *)((char *)thumb->data + (size_t)y * thumb->stride),
			(size_t)thumb->width, rgba + (size_t)y * thumb->width * 4);
	}
	png_init_io(png, f);
	png_set_IHDR(png, info, (png_uint_32)thumb->width, (png_uint_32)thumb->height, 8,
		PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
		PNG_FILTER_TYPE_DEFAULT);
	char when[32];
	snprintf(when, sizeof(when), "%lld", (long long)mtime);
	png_text text[3] = {
		{.compression = PNG_TEXT_COMPRESSION_NONE, .key = "Thumb::URI", .text = uri},
		{.compression = PNG_TEXT_COMPRESSION_NONE, .key = "Thumb::MTime", .text = when},
		{.compression = PNG_TEXT_COMPRESSION_NONE, .key = "Software", .text = "flameui"},
	};
	png_set_text(png, info, text, 3);
	png_write_info(png, info);
	for (int y = 0; y < thumb->height; y++) {
		png_write_row(png, rgba + (size_t)y * thumb->width * 4);
	}
	png_write_end(png, NULL);
	ok = true;
out:
	png_destroy_write_struct(&png, &info);
	free(rgba);
	fchmod(fileno(f), 0600);
	if (fclose(f) != 0) {
		ok = false;
	}
	if (ok) {
		ok = rename(tmp, file) == 0;
	}
	if (!ok) {
		unlink(tmp);
	}
}
