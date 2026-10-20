#ifndef FLAME_XSERVER_WIRE_H
#define FLAME_XSERVER_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "util/strbuf.h"

// X11 on the wire: every client picks its byte order at connection setup,
// everything after that comes and goes in that order. reads past the end
// of a request give 0, the handlers check lengths where it matters

struct xin {
	const uint8_t *data;
	size_t len;
	bool swap; // the client's byte order isn't ours
};

static inline uint8_t xin8(const struct xin *in, size_t at) {
	return at < in->len ? in->data[at] : 0;
}

static inline uint16_t xin16(const struct xin *in, size_t at) {
	if (at + 2 > in->len) {
		return 0;
	}
	uint16_t v;
	memcpy(&v, in->data + at, 2);
	return in->swap ? __builtin_bswap16(v) : v;
}

static inline uint32_t xin32(const struct xin *in, size_t at) {
	if (at + 4 > in->len) {
		return 0;
	}
	uint32_t v;
	memcpy(&v, in->data + at, 4);
	return in->swap ? __builtin_bswap32(v) : v;
}

static inline uint64_t xin64(const struct xin *in, size_t at) {
	return (uint64_t)xin32(in, at) | (uint64_t)xin32(in, at + 4) << 32;
}

static inline int16_t xin16s(const struct xin *in, size_t at) {
	return (int16_t)xin16(in, at);
}

// appends in the client's byte order
struct xout {
	struct strbuf buf;
	bool swap;
};

static inline void xout_init(struct xout *out, bool swap) {
	strbuf_init(&out->buf);
	out->swap = swap;
}

static inline void xout_finish(struct xout *out) {
	strbuf_finish(&out->buf);
}

static inline void xout8(struct xout *out, uint8_t v) {
	strbuf_append(&out->buf, (const char *)&v, 1);
}

static inline void xout16(struct xout *out, uint16_t v) {
	v = out->swap ? __builtin_bswap16(v) : v;
	strbuf_append(&out->buf, (const char *)&v, 2);
}

static inline void xout32(struct xout *out, uint32_t v) {
	v = out->swap ? __builtin_bswap32(v) : v;
	strbuf_append(&out->buf, (const char *)&v, 4);
}

static inline void xout_bytes(struct xout *out, const void *data, size_t len) {
	strbuf_append(&out->buf, data, len);
}

static inline void xout_zero(struct xout *out, size_t len) {
	static const char zeros[32];
	while (len > 0) {
		size_t n = len < sizeof(zeros) ? len : sizeof(zeros);
		strbuf_append(&out->buf, zeros, n);
		len -= n;
	}
}

static inline void xout64(struct xout *out, uint64_t v) {
	xout32(out, (uint32_t)v);
	xout32(out, (uint32_t)(v >> 32));
}

// up to the next multiple of 4
static inline void xout_pad(struct xout *out) {
	xout_zero(out, (4 - out->buf.len % 4) % 4);
}

static inline size_t xpad(size_t len) {
	return (len + 3) & ~(size_t)3;
}

#endif
