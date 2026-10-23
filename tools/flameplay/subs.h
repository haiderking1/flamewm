#ifndef FLAMEPLAY_SUBS_H
#define FLAMEPLAY_SUBS_H

#include <cairo.h>
#include <stdbool.h>
#include <stddef.h>

struct AVSubtitle;

// a video's subtitles, one track of them at a time: text ones (srt, ass,
// webvtt... as ass, the way ffmpeg's decoders give them) drawn by libass,
// a plain track in flame's own style, a styled one as it was made. or
// pictures of text (blu-ray's, dvd's). added from the engine's thread as
// they're decoded, drawn from the window's

struct subs;

struct subs *subs_create(void);
void subs_destroy(struct subs *subs);

// a font a styled track uses, brought with the file (an attachment)
void subs_add_font(struct subs *subs, const char *name, const void *data, size_t size);
// a new track: what was there gone. header: a styled track's own (its ass
// header), NULL for a plain one. width x height: the video's
void subs_reset(struct subs *subs, const char *header, int width, int height);
// a decoded subtitle, at start seconds (pictures: on a canvas
// canvas_w x canvas_h, theirs). the same one again (seeked back over) is
// kept once
void subs_add(struct subs *subs, const struct AVSubtitle *sub, double start, int canvas_w,
	int canvas_h);

// what's shown at t made (w x h window coords at the window's scale, sizes
// in pixels): true when it's not what was drawn last
bool subs_update(struct subs *subs, double t, int width, int height);
// that drawn at x, y (window coords), lifted by lift (over the bar): false
// when there was nothing
bool subs_draw(struct subs *subs, cairo_t *cr, double x, double y, double lift);
// anything on at t, or coming
bool subs_any(struct subs *subs);

#endif
