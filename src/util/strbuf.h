#ifndef FLAME_UTIL_STRBUF_H
#define FLAME_UTIL_STRBUF_H

#include <stdbool.h>
#include <stddef.h>

// a growable byte string. out of memory sets failed and drops further
// appends, so callers check once at the end instead of after every call
struct strbuf {
	char *data; // nul terminated whenever len > 0
	size_t len, cap;
	bool failed;
};

void strbuf_init(struct strbuf *buf);
void strbuf_finish(struct strbuf *buf);
void strbuf_reset(struct strbuf *buf);

void strbuf_append(struct strbuf *buf, const char *data, size_t len);
void strbuf_printf(struct strbuf *buf, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
// as a quoted json string, NULL becomes null. bytes past 0x7f pass through,
// titles are utf-8 already
void strbuf_json_string(struct strbuf *buf, const char *str);
// drop the first count bytes, what's left moves to the front
void strbuf_consume(struct strbuf *buf, size_t count);

bool strbuf_equal(const struct strbuf *a, const struct strbuf *b);

#endif
