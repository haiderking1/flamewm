#ifndef FLAMESEARCH_CALC_H
#define FLAMESEARCH_CALC_H

#include <stdbool.h>
#include <stddef.h>

// what's typed as sums: 2+2, (3 * 4.5) / 2, 2^10, 15% of nothing, sqrt(2),
// 12 × 3. true with the answer in out (as it'd be written) when the whole
// of it is one and it does some working: a number alone isn't a sum
bool calc(const char *text, char *out, size_t size);

#endif
