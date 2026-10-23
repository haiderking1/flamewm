#ifndef FLAMEIPC_JSON_H
#define FLAMEIPC_JSON_H

#include <stdbool.h>
#include <stddef.h>

// just enough json for flame's ipc replies: parsed into a tree, read by
// path. a missing key reads as null, never a crash

enum json_type { JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT };

struct json {
	enum json_type type;
	bool boolean;
	double number;
	char *string;
	// arrays and objects: children, keys for objects
	struct json *items;
	char **keys;
	size_t len;
};

// NULL when it isn't json
struct json *json_parse(const char *text);
void json_free(struct json *json);

const struct json *json_get(const struct json *object, const char *key);
const struct json *json_at(const struct json *array, size_t i);
const char *json_str(const struct json *json, const char *fallback);
double json_num(const struct json *json, double fallback);
bool json_bool(const struct json *json, bool fallback);

#endif
