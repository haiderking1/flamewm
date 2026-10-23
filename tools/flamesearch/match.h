#ifndef FLAMESEARCH_MATCH_H
#define FLAMESEARCH_MATCH_H

#include <stdbool.h>

// how well what's typed matches a name, both folded (lower case, as
// fold() makes them): 0 for not at all, the higher the better. the whole
// name, then its start, a word's start ("code" in "Visual Studio Code"),
// its words' first letters ("vsc"), anywhere in it, then (fuzzy) its
// letters in order with others between
int match(const char *query, const char *name, bool fuzzy);

// text folded for match: lower case (any language's), in out
void fold(const char *text, char *out, int size);

#endif
