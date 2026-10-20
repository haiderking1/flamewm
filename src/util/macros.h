#ifndef FLAME_UTIL_MACROS_H
#define FLAME_UTIL_MACROS_H

#define LENGTH(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(x, lo, hi) MIN(MAX((x), (lo)), (hi))

#endif
