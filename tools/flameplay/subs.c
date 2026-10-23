#include "subs.h"

#include <ass/ass.h>
#include <libavcodec/avcodec.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// a plain track's look: inter, white, a soft dark edge and shadow, low in
// the picture. sizes are of a 288 high frame (ass's own default), scaled
// to the video
static const char plain_header[] =
	"[Script Info]\n"
	"ScriptType: v4.00+\n"
	"PlayResX: 512\n"
	"PlayResY: 288\n"
	"ScaledBorderAndShadow: yes\n"
	"WrapStyle: 0\n"
	"\n"
	"[V4+ Styles]\n"
	"Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, "
	"BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, "
	"BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
	"Style: Default,Inter,17,&H00FFFFFF,&H00FFFFFF,&H90000000,&HA0000000,600,0,0,0,100,100,0,"
	"0,1,0.7,0.9,2,32,32,14,1\n"
	"\n"
	"[Events]\n"
	"Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";

// a picture subtitle: its rects, shown from start till end (INFINITY till
// the next one says)
struct pic {
	int x, y;
	cairo_surface_t *surface;
};

struct pic_event {
	double start, end;
	int canvas_w, canvas_h;
	struct pic *pics;
	int npics;
};

struct subs {
	pthread_mutex_t lock;
	ASS_Library *library;
	ASS_Renderer *renderer;
	ASS_Track *track;
	bool fonts_set;
	int video_w, video_h;
	int read_order;
	// what's been added, to keep each one once: a hash of its time and text
	uint64_t *seen;
	size_t nseen, seen_cap;
	struct pic_event *events;
	size_t nevents, events_cap;

	// made by subs_update for subs_draw
	ASS_Image *images;
	int width, height; // pixels
	struct pic_event *shown[8];
	int nshown;
};

// libass's own errors only, not what it found and how it's set up
static void ass_message(int level, const char *fmt, va_list args, void *data) {
	if (level <= 1) {
		fprintf(stderr, "flameplay: subtitles: ");
		vfprintf(stderr, fmt, args);
		fprintf(stderr, "\n");
	}
}

struct subs *subs_create(void) {
	struct subs *s = calloc(1, sizeof(*s));
	if (!s) {
		return NULL;
	}
	pthread_mutex_init(&s->lock, NULL);
	s->library = ass_library_init();
	if (s->library) {
		ass_set_message_cb(s->library, ass_message, NULL);
	}
	s->renderer = s->library ? ass_renderer_init(s->library) : NULL;
	if (!s->renderer) {
		subs_destroy(s);
		return NULL;
	}
	// fonts a styled track brings with it (a file's attachments)
	ass_set_extract_fonts(s->library, 1);
	return s;
}

static void events_free(struct subs *s) {
	for (size_t i = 0; i < s->nevents; i++) {
		for (int j = 0; j < s->events[i].npics; j++) {
			cairo_surface_destroy(s->events[i].pics[j].surface);
		}
		free(s->events[i].pics);
	}
	s->nevents = 0;
	s->nshown = 0;
}

void subs_destroy(struct subs *s) {
	if (!s) {
		return;
	}
	events_free(s);
	free(s->events);
	free(s->seen);
	if (s->track) {
		ass_free_track(s->track);
	}
	if (s->renderer) {
		ass_renderer_done(s->renderer);
	}
	if (s->library) {
		ass_library_done(s->library);
	}
	pthread_mutex_destroy(&s->lock);
	free(s);
}

void subs_add_font(struct subs *s, const char *name, const void *data, size_t size) {
	pthread_mutex_lock(&s->lock);
	ass_add_font(s->library, name, data, (int)size);
	s->fonts_set = false; // looked for again with it
	pthread_mutex_unlock(&s->lock);
}

void subs_reset(struct subs *s, const char *header, int width, int height) {
	pthread_mutex_lock(&s->lock);
	if (s->track) {
		ass_free_track(s->track);
	}
	events_free(s);
	s->nseen = 0;
	s->read_order = 0;
	s->images = NULL;
	s->video_w = width;
	s->video_h = height;
	s->track = ass_new_track(s->library);
	if (s->track) {
		const char *h = header ? header : plain_header;
		ass_process_codec_private(s->track, (char *)h, (int)strlen(h));
	}
	pthread_mutex_unlock(&s->lock);
}

static uint64_t hash(uint64_t h, const void *data, size_t len) {
	const unsigned char *p = data;
	for (size_t i = 0; i < len; i++) {
		h = (h ^ p[i]) * 0x100000001b3ull;
	}
	return h;
}

// true the first time this one's seen
static bool first_time(struct subs *s, uint64_t key) {
	for (size_t i = 0; i < s->nseen; i++) {
		if (s->seen[i] == key) {
			return false;
		}
	}
	if (s->nseen == s->seen_cap) {
		size_t cap = s->seen_cap ? s->seen_cap * 2 : 256;
		uint64_t *grown = realloc(s->seen, cap * sizeof(*grown));
		if (!grown) {
			return true;
		}
		s->seen = grown;
		s->seen_cap = cap;
	}
	s->seen[s->nseen++] = key;
	return true;
}

// a text subtitle's line (ReadOrder,Layer,Style,... as ffmpeg gives it)
// to libass, under a read order of ours: ffmpeg's starts over after a seek
static void add_text(struct subs *s, const char *ass, long long start_ms, long long dur_ms) {
	const char *rest = strchr(ass, ',');
	if (!rest || !s->track) {
		return;
	}
	rest++;
	uint64_t key = hash(hash(0xcbf29ce484222325ull, &start_ms, sizeof(start_ms)), rest,
		strlen(rest));
	if (!first_time(s, key)) {
		return;
	}
	size_t len = strlen(rest) + 16;
	char *line = malloc(len);
	if (!line) {
		return;
	}
	int n = snprintf(line, len, "%d,%s", ++s->read_order, rest);
	ass_process_chunk(s->track, line, n, start_ms, dur_ms);
	free(line);
}

// a picture subtitle's rect (palette indices) as cairo's premultiplied argb
static cairo_surface_t *rect_surface(const AVSubtitleRect *r) {
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, r->w, r->h);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surface);
		return NULL;
	}
	cairo_surface_flush(surface);
	uint8_t *dst = cairo_image_surface_get_data(surface);
	int stride = cairo_image_surface_get_stride(surface);
	const uint32_t *palette = (const uint32_t *)r->data[1];
	for (int y = 0; y < r->h; y++) {
		uint32_t *row = (uint32_t *)(dst + (size_t)y * (size_t)stride);
		const uint8_t *src = r->data[0] + (size_t)y * (size_t)r->linesize[0];
		for (int x = 0; x < r->w; x++) {
			uint32_t c = palette[src[x]];
			uint32_t a = c >> 24;
			uint32_t red = (c >> 16 & 0xff) * a / 255, green = (c >> 8 & 0xff) * a / 255,
				blue = (c & 0xff) * a / 255;
			row[x] = a << 24 | red << 16 | green << 8 | blue;
		}
	}
	cairo_surface_mark_dirty(surface);
	return surface;
}

static void add_pictures(struct subs *s, const AVSubtitle *sub, double start, double end,
		int canvas_w, int canvas_h) {
	long long start_ms = llround(start * 1000);
	uint64_t key = hash(0x9e3779b97f4a7c15ull, &start_ms, sizeof(start_ms));
	if (!first_time(s, key)) {
		return;
	}
	// one that's on till the next: this is the next
	for (size_t i = 0; i < s->nevents; i++) {
		if (isinf(s->events[i].end) && s->events[i].start < start) {
			s->events[i].end = start;
		}
	}
	if (!sub->num_rects) {
		return; // only the end of the last one
	}
	if (s->nevents == s->events_cap) {
		size_t cap = s->events_cap ? s->events_cap * 2 : 64;
		struct pic_event *grown = realloc(s->events, cap * sizeof(*grown));
		if (!grown) {
			return;
		}
		s->events = grown;
		s->events_cap = cap;
	}
	struct pic_event *e = &s->events[s->nevents];
	*e = (struct pic_event){.start = start, .end = end, .canvas_w = canvas_w,
		.canvas_h = canvas_h};
	e->pics = calloc(sub->num_rects, sizeof(*e->pics));
	if (!e->pics) {
		return;
	}
	for (unsigned i = 0; i < sub->num_rects; i++) {
		const AVSubtitleRect *r = sub->rects[i];
		cairo_surface_t *surface = r->type == SUBTITLE_BITMAP && r->w > 0 && r->h > 0
			? rect_surface(r) : NULL;
		if (surface) {
			e->pics[e->npics++] = (struct pic){r->x, r->y, surface};
		}
	}
	s->nevents++;
}

void subs_add(struct subs *s, const AVSubtitle *sub, double start, int canvas_w, int canvas_h) {
	pthread_mutex_lock(&s->lock);
	start += sub->start_display_time / 1000.0;
	// no end said (or forever): till the next one
	bool open = sub->end_display_time == 0 || sub->end_display_time >= UINT32_MAX / 2
		|| sub->end_display_time <= sub->start_display_time;
	double length = open ? INFINITY
		: (sub->end_display_time - sub->start_display_time) / 1000.0;
	bool pictures = !sub->num_rects;
	for (unsigned i = 0; i < sub->num_rects; i++) {
		const AVSubtitleRect *r = sub->rects[i];
		if (r->type == SUBTITLE_ASS && r->ass) {
			add_text(s, r->ass, llround(start * 1000), open ? 5000 : llround(length * 1000));
		} else if (r->type == SUBTITLE_BITMAP) {
			pictures = true;
		}
	}
	if (pictures) {
		add_pictures(s, sub, start, start + length, canvas_w, canvas_h);
	}
	pthread_mutex_unlock(&s->lock);
}

bool subs_update(struct subs *s, double t, int width, int height) {
	pthread_mutex_lock(&s->lock);
	bool changed = width != s->width || height != s->height;
	s->width = width;
	s->height = height;
	ASS_Image *images = NULL;
	if (s->track && s->track->n_events && width > 0 && height > 0) {
		if (!s->fonts_set) {
			// the system's (fontconfig), and what the track brought
			ass_set_fonts(s->renderer, NULL, "sans-serif", ASS_FONTPROVIDER_AUTODETECT, NULL, 1);
			s->fonts_set = true;
		}
		ass_set_frame_size(s->renderer, width, height);
		ass_set_storage_size(s->renderer, s->video_w, s->video_h);
		int change = 0;
		images = ass_render_frame(s->renderer, s->track, llround(t * 1000), &change);
		changed |= change != 0;
	}
	changed |= (images != NULL) != (s->images != NULL);
	s->images = images;
	// the pictures on now
	struct pic_event *shown[8];
	int nshown = 0;
	for (size_t i = 0; i < s->nevents && nshown < 8; i++) {
		if (s->events[i].start <= t && t < s->events[i].end) {
			shown[nshown++] = &s->events[i];
		}
	}
	changed |= nshown != s->nshown || memcmp(shown, s->shown, sizeof(shown[0]) * (size_t)nshown);
	memcpy(s->shown, shown, sizeof(shown[0]) * (size_t)nshown);
	s->nshown = nshown;
	pthread_mutex_unlock(&s->lock);
	return changed;
}

bool subs_draw(struct subs *s, cairo_t *cr, double x, double y, double lift) {
	pthread_mutex_lock(&s->lock);
	if (!s->images && !s->nshown) {
		pthread_mutex_unlock(&s->lock);
		return false;
	}
	// in pixels, on whole ones: sharp at any scale
	cairo_save(cr);
	double dx = x, dy = y - lift;
	cairo_user_to_device(cr, &dx, &dy);
	cairo_identity_matrix(cr);
	cairo_translate(cr, round(dx), round(dy));
	for (int i = 0; i < s->nshown; i++) {
		struct pic_event *e = s->shown[i];
		double sx = (double)s->width / (e->canvas_w > 0 ? e->canvas_w : s->video_w);
		double sy = (double)s->height / (e->canvas_h > 0 ? e->canvas_h : s->video_h);
		for (int j = 0; j < e->npics; j++) {
			cairo_save(cr);
			cairo_scale(cr, sx, sy);
			cairo_set_source_surface(cr, e->pics[j].surface, e->pics[j].x, e->pics[j].y);
			cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
			cairo_paint(cr);
			cairo_restore(cr);
		}
	}
	for (ASS_Image *img = s->images; img; img = img->next) {
		if (img->w <= 0 || img->h <= 0) {
			continue;
		}
		cairo_surface_t *mask = cairo_image_surface_create(CAIRO_FORMAT_A8, img->w, img->h);
		if (cairo_surface_status(mask) != CAIRO_STATUS_SUCCESS) {
			cairo_surface_destroy(mask);
			continue;
		}
		cairo_surface_flush(mask);
		uint8_t *dst = cairo_image_surface_get_data(mask);
		int stride = cairo_image_surface_get_stride(mask);
		for (int row = 0; row < img->h; row++) {
			memcpy(dst + (size_t)row * (size_t)stride, img->bitmap + (size_t)row * (size_t)img->stride,
				(size_t)img->w);
		}
		cairo_surface_mark_dirty(mask);
		// rrggbbaa, its alpha a transparency
		uint32_t c = img->color;
		cairo_set_source_rgba(cr, (c >> 24) / 255.0, (c >> 16 & 0xff) / 255.0,
			(c >> 8 & 0xff) / 255.0, (255 - (c & 0xff)) / 255.0);
		cairo_mask_surface(cr, mask, img->dst_x, img->dst_y);
		cairo_surface_destroy(mask);
	}
	cairo_restore(cr);
	pthread_mutex_unlock(&s->lock);
	return true;
}

bool subs_any(struct subs *s) {
	pthread_mutex_lock(&s->lock);
	bool any = (s->track && s->track->n_events) || s->nevents;
	pthread_mutex_unlock(&s->lock);
	return any;
}
