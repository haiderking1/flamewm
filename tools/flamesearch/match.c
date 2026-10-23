#include "match.h"

#include <glib.h>
#include <stdio.h>
#include <string.h>

void fold(const char *text, char *out, int size) {
	char *folded = g_utf8_casefold(text, -1);
	snprintf(out, (size_t)size, "%s", folded ? folded : text);
	g_free(folded);
}

// a word starts at i: the name's start, or after a space or a mark
static bool word_start(const char *name, size_t i) {
	if (i == 0) {
		return true;
	}
	char before = name[i - 1];
	return before == ' ' || before == '-' || before == '_' || before == '.' || before == '/'
		|| before == '(' || before == ':';
}

// the query's letters each a word's first, in order
static bool initials(const char *query, const char *name) {
	size_t q = 0, len = strlen(name);
	for (size_t i = 0; i < len && query[q]; i++) {
		if (word_start(name, i) && name[i] == query[q]) {
			q++;
		}
	}
	return !query[q];
}

// the query's letters in order: how spread out (0 for not all there)
static int in_order(const char *query, const char *name) {
	const char *at = name;
	int first = -1, last = 0;
	for (const char *q = query; *q; q++) {
		const char *found = strchr(at, *q);
		if (!found) {
			return 0;
		}
		if (first < 0) {
			first = (int)(found - name);
		}
		last = (int)(found - name);
		at = found + 1;
	}
	return last - first + 1;
}

int match(const char *query, const char *name, bool fuzzy) {
	size_t q = strlen(query), n = strlen(name);
	if (!q || q > n) {
		return 0;
	}
	// shorter names that match the same way first: "code" over "codeblocks"
	int shorter = n - q < 60 ? 60 - (int)(n - q) : 0;
	if (!strcmp(query, name)) {
		return 1000;
	}
	if (!strncmp(query, name, q)) {
		return 900 + shorter;
	}
	for (const char *at = strstr(name + 1, query); at; at = strstr(at + 1, query)) {
		if (word_start(name, (size_t)(at - name))) {
			return 750 + shorter;
		}
	}
	if (q >= 2 && initials(query, name)) {
		return 650 + shorter;
	}
	if (strstr(name, query)) {
		return 500 + shorter;
	}
	if (fuzzy && q >= 3) {
		int spread = in_order(query, name);
		if (spread && spread <= (int)q * 3) {
			return 200 + (int)(100 * q / (size_t)spread);
		}
	}
	return 0;
}
