#include "json.h"

#include <stdlib.h>
#include <string.h>

struct parser {
	const char *p;
	bool failed;
};

static void skip(struct parser *ps) {
	while (*ps->p == ' ' || *ps->p == '\n' || *ps->p == '\t' || *ps->p == '\r') {
		ps->p++;
	}
}

static bool parse_value(struct parser *ps, struct json *out);

static char *parse_string(struct parser *ps) {
	if (*ps->p != '"') {
		ps->failed = true;
		return NULL;
	}
	ps->p++;
	size_t cap = 32, len = 0;
	char *s = malloc(cap);
	while (s && *ps->p && *ps->p != '"') {
		char c = *ps->p++;
		if (c == '\\') {
			char e = *ps->p++;
			c = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == 'b' ? '\b'
				: e == 'f' ? '\f' : e;
			if (e == 'u') {
				// flame only escapes control characters this way
				c = (char)strtol((char[5]){ps->p[0], ps->p[1], ps->p[2], ps->p[3], 0}, NULL, 16);
				ps->p += 4;
			}
		}
		if (len + 2 > cap) {
			cap *= 2;
			char *grown = realloc(s, cap);
			if (!grown) {
				free(s);
				s = NULL;
				break;
			}
			s = grown;
		}
		s[len++] = c;
	}
	if (!s || *ps->p != '"') {
		free(s);
		ps->failed = true;
		return NULL;
	}
	ps->p++;
	s[len] = '\0';
	return s;
}

static bool append(struct json *out, struct json *item, char *key) {
	struct json *items = realloc(out->items, (out->len + 1) * sizeof(*items));
	char **keys = out->type == JSON_OBJECT
		? realloc(out->keys, (out->len + 1) * sizeof(*keys)) : out->keys;
	if (!items || (out->type == JSON_OBJECT && !keys)) {
		if (items) {
			out->items = items;
		}
		return false;
	}
	out->items = items;
	out->keys = keys;
	out->items[out->len] = *item;
	if (keys) {
		keys[out->len] = key;
	}
	out->len++;
	return true;
}

static bool parse_value(struct parser *ps, struct json *out) {
	skip(ps);
	memset(out, 0, sizeof(*out));
	const char *p = ps->p;
	if (*p == '{' || *p == '[') {
		bool object = *p == '{';
		out->type = object ? JSON_OBJECT : JSON_ARRAY;
		ps->p++;
		skip(ps);
		if (*ps->p == (object ? '}' : ']')) {
			ps->p++;
			return true;
		}
		for (;;) {
			char *key = NULL;
			if (object) {
				skip(ps);
				key = parse_string(ps);
				skip(ps);
				if (!key || *ps->p != ':') {
					free(key);
					ps->failed = true;
					return false;
				}
				ps->p++;
			}
			struct json item;
			if (!parse_value(ps, &item) || !append(out, &item, key)) {
				free(key);
				ps->failed = true;
				return false;
			}
			skip(ps);
			if (*ps->p == ',') {
				ps->p++;
				continue;
			}
			if (*ps->p == (object ? '}' : ']')) {
				ps->p++;
				return true;
			}
			ps->failed = true;
			return false;
		}
	}
	if (*p == '"') {
		out->type = JSON_STRING;
		out->string = parse_string(ps);
		return out->string != NULL;
	}
	if (!strncmp(p, "true", 4) || !strncmp(p, "false", 5)) {
		out->type = JSON_BOOL;
		out->boolean = *p == 't';
		ps->p += out->boolean ? 4 : 5;
		return true;
	}
	if (!strncmp(p, "null", 4)) {
		ps->p += 4;
		return true;
	}
	char *end;
	out->type = JSON_NUMBER;
	out->number = strtod(p, &end);
	if (end == p) {
		ps->failed = true;
		return false;
	}
	ps->p = end;
	return true;
}

static void json_clear(struct json *json) {
	for (size_t i = 0; i < json->len; i++) {
		json_clear(&json->items[i]);
		if (json->keys) {
			free(json->keys[i]);
		}
	}
	free(json->items);
	free(json->keys);
	free(json->string);
}

struct json *json_parse(const char *text) {
	struct json *json = calloc(1, sizeof(*json));
	struct parser ps = {.p = text};
	if (!json || !parse_value(&ps, json) || ps.failed) {
		if (json) {
			json_clear(json);
			free(json);
		}
		return NULL;
	}
	return json;
}

void json_free(struct json *json) {
	if (json) {
		json_clear(json);
		free(json);
	}
}

const struct json *json_get(const struct json *object, const char *key) {
	if (!object || object->type != JSON_OBJECT) {
		return NULL;
	}
	for (size_t i = 0; i < object->len; i++) {
		if (!strcmp(object->keys[i], key)) {
			return &object->items[i];
		}
	}
	return NULL;
}

const struct json *json_at(const struct json *array, size_t i) {
	return array && array->type == JSON_ARRAY && i < array->len ? &array->items[i] : NULL;
}

const char *json_str(const struct json *json, const char *fallback) {
	return json && json->type == JSON_STRING ? json->string : fallback;
}

double json_num(const struct json *json, double fallback) {
	return json && json->type == JSON_NUMBER ? json->number : fallback;
}

bool json_bool(const struct json *json, bool fallback) {
	return json && json->type == JSON_BOOL ? json->boolean : fallback;
}
