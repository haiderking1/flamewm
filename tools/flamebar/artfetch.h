#ifndef FLAMEBAR_ARTFETCH_H
#define FLAMEBAR_ARTFETCH_H

#include <stdbool.h>
#include <stddef.h>

// a player's art on the web (an https artUrl, a song's cover at full
// size): fetched once into the cache (~/.cache/flame/art, by the url),
// read from there after. one at a time, on a thread of its own

struct art_fetch;

// http:// or https://
bool art_is_web(const char *url);
// where url's picture is kept (whether it's there yet or not)
bool art_cached_path(const char *url, char *out, size_t size);
// fetched into art_cached_path's file on a thread, wake(data) called from
// it when it's done, either way. NULL when it can't start
struct art_fetch *art_fetch_start(const char *url, void (*wake)(void *data), void *data);
// done (got it or not): true, *ok saying which. not done: false
bool art_fetch_done(struct art_fetch *fetch, bool *ok);
// let go: one that's done is freed, one still going frees itself when
// it's through
void art_fetch_free(struct art_fetch *fetch);

#endif
