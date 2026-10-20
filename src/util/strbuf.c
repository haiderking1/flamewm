#include "util/strbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void strbuf_init(struct strbuf *buf) {
	*buf = (struct strbuf){0};
}

void strbuf_finish(struct strbuf *buf) {
	free(buf->data);
	strbuf_init(buf);
}

void strbuf_reset(struct strbuf *buf) {
	buf->len = 0;
	buf->failed = false;
	if (buf->data) {
		buf->data[0] = '\0';
	}
}

// room for extra more bytes plus the nul
static bool reserve(struct strbuf *buf, size_t extra) {
	if (buf->failed) {
		return false;
	}
	size_t need = buf->len + extra + 1;
	if (need <= buf->cap) {
		return true;
	}
	size_t cap = buf->cap ? buf->cap : 256;
	while (cap < need) {
		cap *= 2;
	}
	char *data = realloc(buf->data, cap);
	if (!data) {
		buf->failed = true;
		return false;
	}
	buf->data = data;
	buf->cap = cap;
	return true;
}

void strbuf_append(struct strbuf *buf, const char *data, size_t len) {
	if (!reserve(buf, len)) {
		return;
	}
	memcpy(buf->data + buf->len, data, len);
	buf->len += len;
	buf->data[buf->len] = '\0';
}

void strbuf_printf(struct strbuf *buf, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	int len = vsnprintf(NULL, 0, fmt, args);
	va_end(args);
	if (len < 0 || !reserve(buf, (size_t)len)) {
		buf->failed |= len < 0;
		return;
	}
	va_start(args, fmt);
	vsnprintf(buf->data + buf->len, (size_t)len + 1, fmt, args);
	va_end(args);
	buf->len += (size_t)len;
}

void strbuf_json_string(struct strbuf *buf, const char *str) {
	if (!str) {
		strbuf_append(buf, "null", 4);
		return;
	}
	strbuf_append(buf, "\"", 1);
	for (const unsigned char *c = (const unsigned char *)str; *c; c++) {
		switch (*c) {
		case '"': strbuf_append(buf, "\\\"", 2); break;
		case '\\': strbuf_append(buf, "\\\\", 2); break;
		case '\n': strbuf_append(buf, "\\n", 2); break;
		case '\r': strbuf_append(buf, "\\r", 2); break;
		case '\t': strbuf_append(buf, "\\t", 2); break;
		default:
			if (*c < 0x20) {
				strbuf_printf(buf, "\\u%04x", *c);
			} else {
				strbuf_append(buf, (const char *)c, 1);
			}
		}
	}
	strbuf_append(buf, "\"", 1);
}

void strbuf_consume(struct strbuf *buf, size_t count) {
	if (count >= buf->len) {
		strbuf_reset(buf);
		return;
	}
	memmove(buf->data, buf->data + count, buf->len - count);
	buf->len -= count;
	buf->data[buf->len] = '\0';
}

bool strbuf_equal(const struct strbuf *a, const struct strbuf *b) {
	return a->len == b->len && (a->len == 0 || !memcmp(a->data, b->data, a->len));
}
