#include "control.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define GAP 12
#define TILE_H 64
#define HALF ((CONTROL_W - GAP) / 2.0)
#define SOUND_H 84
#define ART_W (CONTROL_W - 28) // its art across it, a video's shape
#define ART_H 176
#define MEDIA_H (14 + ART_H + 12 + 56 + 30 + 52) // now playing opened

// ---- icons, drawn at cx, cy in a s box ----

static void icon_wifi(cairo_t *cr, double cx, double cy, double s) {
	for (int i = 1; i <= 3; i++) {
		cairo_new_sub_path(cr);
		cairo_arc(cr, cx, cy + s * 0.3, s * 0.22 * i, -M_PI * 0.75, -M_PI * 0.25);
	}
	cairo_set_line_width(cr, s * 0.1);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_stroke(cr);
	cairo_arc(cr, cx, cy + s * 0.3, s * 0.07, 0, 2 * M_PI);
	cairo_fill(cr);
}

// a wired network: three boxes joined
static void icon_wired(cairo_t *cr, double cx, double cy, double s) {
	double b = s * 0.26;
	cairo_rectangle(cr, cx - b / 2, cy - s * 0.42, b, b);
	cairo_rectangle(cr, cx - s * 0.42, cy + s * 0.16, b, b);
	cairo_rectangle(cr, cx + s * 0.42 - b, cy + s * 0.16, b, b);
	cairo_fill(cr);
	cairo_move_to(cr, cx, cy - s * 0.16);
	cairo_line_to(cr, cx, cy);
	cairo_move_to(cr, cx - s * 0.29, cy + s * 0.16);
	cairo_line_to(cr, cx - s * 0.29, cy);
	cairo_line_to(cr, cx + s * 0.29, cy);
	cairo_line_to(cr, cx + s * 0.29, cy + s * 0.16);
	cairo_set_line_width(cr, s * 0.08);
	cairo_stroke(cr);
}

static void icon_bluetooth(cairo_t *cr, double cx, double cy, double s) {
	double h = s * 0.42, w = s * 0.2;
	cairo_move_to(cr, cx - w, cy - h * 0.5);
	cairo_line_to(cr, cx + w, cy + h * 0.5);
	cairo_line_to(cr, cx, cy + h);
	cairo_line_to(cr, cx, cy - h);
	cairo_line_to(cr, cx + w, cy - h * 0.5);
	cairo_line_to(cr, cx - w, cy + h * 0.5);
	cairo_set_line_width(cr, s * 0.09);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	cairo_stroke(cr);
}

static void icon_speaker(cairo_t *cr, double cx, double cy, double s, int waves) {
	cairo_move_to(cr, cx - s * 0.45, cy - s * 0.14);
	cairo_line_to(cr, cx - s * 0.25, cy - s * 0.14);
	cairo_line_to(cr, cx - s * 0.02, cy - s * 0.36);
	cairo_line_to(cr, cx - s * 0.02, cy + s * 0.36);
	cairo_line_to(cr, cx - s * 0.25, cy + s * 0.14);
	cairo_line_to(cr, cx - s * 0.45, cy + s * 0.14);
	cairo_close_path(cr);
	cairo_fill(cr);
	cairo_set_line_width(cr, s * 0.08);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	for (int i = 1; i <= waves; i++) {
		cairo_new_sub_path(cr);
		cairo_arc(cr, cx, cy, s * 0.16 * i, -M_PI / 4, M_PI / 4);
	}
	cairo_stroke(cr);
	if (waves < 0) {
		// muted: crossed out
		cairo_move_to(cr, cx + s * 0.12, cy - s * 0.14);
		cairo_line_to(cr, cx + s * 0.4, cy + s * 0.14);
		cairo_move_to(cr, cx + s * 0.4, cy - s * 0.14);
		cairo_line_to(cr, cx + s * 0.12, cy + s * 0.14);
		cairo_stroke(cr);
	}
}

static void icon_play(cairo_t *cr, double cx, double cy, double s, bool pause) {
	if (pause) {
		cairo_rectangle(cr, cx - s * 0.3, cy - s * 0.38, s * 0.2, s * 0.76);
		cairo_rectangle(cr, cx + s * 0.1, cy - s * 0.38, s * 0.2, s * 0.76);
	} else {
		cairo_move_to(cr, cx - s * 0.28, cy - s * 0.4);
		cairo_line_to(cr, cx + s * 0.4, cy);
		cairo_line_to(cr, cx - s * 0.28, cy + s * 0.4);
		cairo_close_path(cr);
	}
	cairo_fill(cr);
}

// two triangles: forward, or back (dir -1)
static void icon_skip(cairo_t *cr, double cx, double cy, double s, int dir) {
	for (int i = 0; i < 2; i++) {
		double x0 = cx + dir * (i ? 0 : -s * 0.36);
		cairo_move_to(cr, x0, cy - s * 0.3);
		cairo_line_to(cr, x0 + dir * s * 0.36, cy);
		cairo_line_to(cr, x0, cy + s * 0.3);
		cairo_close_path(cr);
	}
	cairo_fill(cr);
}

// a crescent: a circle with a bite out of it
void icon_moon(cairo_t *cr, double cx, double cy, double s) {
	double r = s * 0.36;
	cairo_push_group(cr);
	cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_arc(cr, cx + r * 0.62, cy - r * 0.5, r * 0.82, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_pop_group_to_source(cr);
	cairo_paint(cr);
}

// ---- tiles ----

// a switch tile: its icon in a circle (lit when on), its name, how it is.
// true when clicked
static bool switch_tile(struct fui *ui, uint64_t id, double x, double y, double w, bool on,
		bool enabled, void (*icon)(cairo_t *cr, double cx, double cy, double s),
		const char *name, const char *sub) {
	cairo_t *cr = fui_cairo(ui);
	double r = 22, cx = x + 10 + r, cy = y + TILE_H / 2.0;
	bool hover = enabled && fui_hover(ui, x, y, w, TILE_H);
	double lit = fui_ease(ui, id, hover ? 1 : 0, 20);
	if (lit > 0.01) {
		fui_fill_rounded(ui, x, y, w, TILE_H, TILE_H / 2.0,
			0xffffff00 | (uint32_t)(0x14 * lit));
	}
	cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
	fui_color(ui, on ? ON : 0xffffff26);
	cairo_fill(cr);
	fui_color(ui, enabled ? TEXT : TEXT_DIM);
	icon(cr, cx, cy, 24);
	double tx = cx + r + 10, tw = x + w - tx - 12;
	shadowed_text(ui, tx, cy - 17, name, F_TITLE, TEXT, FUI_LEFT, tw);
	shadowed_text(ui, tx, cy + 1, sub, F_SUB, TEXT_DIM, FUI_LEFT, tw);
	if (hover) {
		fui_cursor_pointer(ui);
	}
	return enabled && fui_clicked(ui, x, y, w, TILE_H);
}

static void network_tile(struct fui *ui, struct control *c, double x, double y) {
	struct network_now net;
	bool have = network_get(c->network, &net);
	bool wifi = have && (net.wifi || net.wireless);
	const char *sub = !have ? "Unavailable" : wifi && !net.wifi_on ? "Off"
		: net.connected ? net.name : "Not Connected";
	if (switch_tile(ui, fui_id("network", 0), x, y, HALF, have && (wifi ? net.wifi_on
			: net.connected),
			wifi, wifi ? icon_wifi : icon_wired, wifi ? "Wi-Fi" : "Network", sub)) {
		network_set_wifi(c->network, !net.wifi_on);
	}
}

static void bluetooth_tile(struct fui *ui, struct control *c, double x, double y) {
	struct bluetooth_now bt;
	bool have = bluetooth_get(c->bluetooth, &bt);
	const char *sub = !have ? "Unavailable" : !bt.on ? "Off" : bt.device[0] ? bt.device : "On";
	if (switch_tile(ui, fui_id("bluetooth", 0), x, y, HALF, have && bt.on, have, icon_bluetooth,
			"Bluetooth", sub)) {
		bluetooth_set_on(c->bluetooth, !bt.on);
	}
}

// do not disturb: notifications to the list without a card
static void quiet_tile(struct fui *ui, struct control *c, double x, double y) {
	bool have = c->notify != NULL, quiet = notify_quiet(c->notify);
	if (switch_tile(ui, fui_id("quiet", 0), x, y, CONTROL_W, quiet, have, icon_moon,
			"Do Not Disturb", !have ? "Unavailable" : quiet ? "On" : "Off")) {
		notify_set_quiet(c->notify, !quiet);
	}
}

// a round button around an icon, true when clicked
static bool round_button(struct fui *ui, uint64_t id, double cx, double cy, double r) {
	bool hover = fui_hover(ui, cx - r, cy - r, 2 * r, 2 * r);
	double lit = fui_ease(ui, id, hover ? 1 : 0, 20);
	if (lit > 0.01) {
		cairo_arc(fui_cairo(ui), cx, cy, r, 0, 2 * M_PI);
		fui_color(ui, 0xffffff00 | (uint32_t)(0x1f * lit));
		cairo_fill(fui_cairo(ui));
	}
	if (hover) {
		fui_cursor_pointer(ui);
	}
	return fui_clicked(ui, cx - r, cy - r, 2 * r, 2 * r);
}

// a microphone: its head, the stand under it
static void icon_mic(cairo_t *cr, double cx, double cy, double s, bool muted) {
	double w = s * 0.32, h = s * 0.52;
	cairo_new_sub_path(cr);
	cairo_arc(cr, cx, cy - h / 2 + w / 2, w / 2, M_PI, 2 * M_PI);
	cairo_arc(cr, cx, cy + h / 2 - w / 2 - s * 0.1, w / 2, 0, M_PI);
	cairo_close_path(cr);
	cairo_fill(cr);
	cairo_set_line_width(cr, s * 0.08);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_new_sub_path(cr);
	cairo_arc(cr, cx, cy, s * 0.27, 0, M_PI);
	cairo_move_to(cr, cx, cy + s * 0.27);
	cairo_line_to(cr, cx, cy + s * 0.42);
	cairo_stroke(cr);
	if (muted) {
		cairo_move_to(cr, cx - s * 0.36, cy - s * 0.36);
		cairo_line_to(cr, cx + s * 0.36, cy + s * 0.36);
		cairo_stroke(cr);
	}
}

// a chevron: more to see (right), or it's open (down)
static void icon_more(cairo_t *cr, double cx, double cy, double s, bool open) {
	double a = s * 0.18;
	if (open) {
		cairo_move_to(cr, cx - a * 1.4, cy - a * 0.6);
		cairo_line_to(cr, cx, cy + a * 0.8);
		cairo_line_to(cr, cx + a * 1.4, cy - a * 0.6);
	} else {
		cairo_move_to(cr, cx - a * 0.6, cy - a * 1.4);
		cairo_line_to(cr, cx + a * 0.8, cy);
		cairo_line_to(cr, cx - a * 0.6, cy + a * 1.4);
	}
	cairo_set_line_width(cr, s * 0.1);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	cairo_stroke(cr);
}

static double lerp(double a, double b, double t) {
	return a + (b - a) * t;
}

// the art in a rounded w x h, cut to cover it: one picture (the big
// one's, made once) at every size and shape it goes through, so it never
// changes picture as it opens. an empty one without
static void art_rect(struct fui *ui, const struct media_now *now, bool have, double x,
		double y, double w, double h, double radius) {
	cairo_t *cr = fui_cairo(ui);
	cairo_surface_t *picture = have && now->art[0]
		? fui_thumb(ui, now->art, ART_W * 2, ART_H * 2, NULL) : NULL;
	fui_rounded(ui, x, y, w, h, radius);
	if (picture) {
		double pw = cairo_image_surface_get_width(picture);
		double ph = cairo_image_surface_get_height(picture);
		double scale = fmax(w / pw, h / ph);
		cairo_save(cr);
		cairo_clip(cr);
		cairo_translate(cr, x + (w - pw * scale) / 2, y + (h - ph * scale) / 2);
		cairo_scale(cr, scale, scale);
		cairo_set_source_surface(cr, picture, 0, 0);
		cairo_paint(cr);
		cairo_restore(cr);
	} else {
		fui_color(ui, 0xffffff1f);
		cairo_fill(cr);
	}
}

// back, play or pause, forward around mid, cy: s their size, gap apart.
// live: they take clicks
static void media_buttons(struct fui *ui, struct control *c, bool have, bool playing,
		double mid, double cy, double s, double gap, bool live) {
	cairo_t *cr = fui_cairo(ui);
	if (live && have && round_button(ui, fui_id("back", 0), mid - gap, cy, s * 0.9)) {
		media_control(c->media, "Previous");
	}
	if (live && have && round_button(ui, fui_id("playpause", 0), mid, cy, s)) {
		media_control(c->media, "PlayPause");
	}
	if (live && have && round_button(ui, fui_id("next", 0), mid + gap, cy, s * 0.9)) {
		media_control(c->media, "Next");
	}
	fui_color(ui, have ? TEXT : TEXT_DIM);
	icon_skip(cr, mid - gap, cy, s, -1);
	icon_play(cr, mid, cy, s * 1.1, have && playing);
	icon_skip(cr, mid + gap, cy, s, 1);
}

// the tile small: the art, what it is, the buttons under them
static void media_small(struct fui *ui, struct control *c, const struct media_now *now,
		bool have, double x, double y, double h, bool live) {
	double art = 44, ay = y + 14, tw = HALF - 32;
	shadowed_text(ui, x + 16, ay + art + 6, have ? now->title : "Not Playing", F_TITLE, TEXT,
		FUI_LEFT, tw);
	if (have && now->artist[0]) {
		shadowed_text(ui, x + 16, ay + art + 22, now->artist, F_SUB, TEXT_DIM, FUI_LEFT, tw);
	}
	media_buttons(ui, c, have, now->playing, x + HALF / 2, y + h - 24, 18, 44, live);
}

static void clock_text(double t, char *out, size_t size) {
	int s = (int)floor(fmax(t, 0) + 0.5);
	if (s >= 3600) {
		snprintf(out, size, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
	} else {
		snprintf(out, size, "%d:%02d", s / 60, s % 60);
	}
}

// the tile big: the art across its top (wide, a video's shape: a still of
// one shows whole), what it is under it, where it is in it (dragged to move
// there), the buttons
static void media_big(struct fui *ui, struct control *c, const struct media_now *now,
		bool have, double x, double y, double w, double h, bool live) {
	double ty = y + 14 + ART_H + 12, tw = w - 40;
	shadowed_text(ui, x + 20, ty, have ? now->title : "Not Playing", F_BIG_TITLE, TEXT,
		FUI_LEFT, tw);
	if (have && now->artist[0]) {
		shadowed_text(ui, x + 20, ty + 20, now->artist, F_SUB, TEXT_DIM, FUI_LEFT, tw);
	}
	// where it is: the line filled that far, the times under it
	double sx = x + 20, sw = w - 40, sy = ty + 56, th = 6;
	double t = have ? now->position : 0, length = have ? now->length : 0;
	bool seekable = live && have && now->can_seek;
	if (seekable && fui_hover(ui, sx - 6, sy - 10, sw + 12, 20)) {
		fui_cursor_pointer(ui);
	}
	if (seekable && fui_clicked(ui, sx - 6, sy - 10, sw + 12, 20)) {
		c->scrubbing = true;
	}
	if (c->scrubbing) {
		double px, py;
		fui_pointer(ui, &px, &py);
		t = fmax(0, fmin((px - sx) / sw, 1)) * length;
		if (!fui_button_held(ui)) {
			c->scrubbing = false;
			media_seek(c->media, t);
		}
		fui_redraw(ui);
	}
	double frac = length > 0 ? fmin(t / length, 1) : 0;
	fui_fill_rounded(ui, sx, sy - th / 2, sw, th, th / 2, 0xffffff40);
	fui_fill_rounded(ui, sx, sy - th / 2, fmax(sw * frac, th), th, th / 2, TEXT);
	char at[32] = "", left[32] = "-";
	if (length > 0) {
		clock_text(t, at, sizeof(at));
		clock_text(length - t, left + 1, sizeof(left) - 1);
		shadowed_text(ui, sx, sy + 8, at, F_SUB, TEXT_DIM, FUI_LEFT, 0);
		shadowed_text(ui, sx + sw, sy + 8, left, F_SUB, TEXT_DIM, FUI_RIGHT, 0);
	}
	media_buttons(ui, c, have, now->playing, x + w / 2, y + h - 30, 22, 64, live);
}

// while it's open and playing: where it is asked again now and then, the
// time drawn on as it goes
static void media_tick(struct fui *ui, void *data) {
	struct control *c = data;
	if (++c->ticks % 4 == 0) {
		media_refresh(c->media);
	}
	fui_redraw(ui);
}

// the now playing tile, at its place for how open it is (e: small 0, big
// 1, the rect between them): small and big faded across, a click on it
// (not its buttons) opens or closes it
static void media_tile(struct fui *ui, struct control *c, double x, double y, double w,
		double h, double e) {
	cairo_t *cr = fui_cairo(ui);
	struct media_now now;
	bool have = media_get(c->media, &now);
	cairo_save(cr);
	cairo_rectangle(cr, x, y, w, h);
	cairo_clip(cr);
	// the art, one thing growing from the small square to across the top
	art_rect(ui, &now, have, x + lerp(16, 14, e), y + 14, lerp(44, w - 28, e), lerp(44, ART_H, e),
		lerp(10, 18, e));
	// what's around it faded across, a little overlapping: never all gone
	double small = fmax(0, 1 - e * 1.6), big = fmax(0, e * 1.6 - 0.6);
	if (small > 0) {
		cairo_push_group(cr);
		media_small(ui, c, &now, have, x, y, h, e < 0.05);
		cairo_pop_group_to_source(cr);
		cairo_paint_with_alpha(cr, small);
	}
	if (big > 0) {
		cairo_push_group(cr);
		media_big(ui, c, &now, have, x, y, w, h, e > 0.95);
		cairo_pop_group_to_source(cr);
		cairo_paint_with_alpha(cr, big);
	}
	cairo_restore(cr);
	if (have && fui_hover(ui, x, y, w, h)) {
		fui_cursor_pointer(ui);
	}
	if (have && !c->scrubbing && fui_clicked(ui, x, y, w, h)) {
		c->media_open = !c->media_open;
		fui_redraw(ui);
	}
	if (!have) {
		c->media_open = false;
	}
	if (c->media_open && now.playing) {
		fui_after(ui, 250, media_tick, c);
	}
}

#define DEVICE_H 34
#define DEVICES_MAX 16

#define LIST_TITLE_H 24

// a side's devices, listed under its slider when it's open: how tall
static double list_height(struct control *c, enum sound_side side) {
	struct sound_device devices[DEVICES_MAX];
	int n = sound_devices(c->sound, side, devices, DEVICES_MAX);
	return LIST_TITLE_H + (n ? n : 1) * DEVICE_H + 12;
}

// the devices, in the tile under its slider, at y, shown by open (0..1):
// the one in use ticked, a click uses another
static void device_list(struct fui *ui, struct control *c, enum sound_side side, double x,
		double y, double open) {
	cairo_t *cr = fui_cairo(ui);
	struct sound_device devices[DEVICES_MAX];
	int n = sound_devices(c->sound, side, devices, DEVICES_MAX);
	uint32_t alpha = (uint32_t)(0xff * open), dim = alpha * (TEXT_DIM & 0xff) / 0xff;
	// its title, dim: what these are
	shadowed_text(ui, x + 20, y + 4, side == SOUND_OUTPUT ? "Output" : "Input", F_LIST_TITLE,
		0xffffff00 | dim, FUI_LEFT, 0);
	y += LIST_TITLE_H;
	if (!n) {
		shadowed_text(ui, x + 20, y + 10, "None", F_SUB, 0xffffff00 | dim, FUI_LEFT, 0);
	}
	for (int i = 0; i < n; i++) {
		double ry = y + 4 + i * DEVICE_H;
		uint64_t id = fui_id("device", (uint64_t)(side * DEVICES_MAX + i));
		bool hover = open > 0.95 && fui_hover(ui, x + 8, ry, CONTROL_W - 16, DEVICE_H);
		double lit = fui_ease(ui, id, hover ? 1 : 0, 20);
		if (lit > 0.01) {
			fui_fill_rounded(ui, x + 8, ry, CONTROL_W - 16, DEVICE_H, 10,
				0xffffff00 | (uint32_t)(0x1a * lit * open));
		}
		if (devices[i].current) {
			cairo_move_to(cr, x + 22, ry + DEVICE_H / 2.0);
			cairo_line_to(cr, x + 26, ry + DEVICE_H / 2.0 + 4);
			cairo_line_to(cr, x + 33, ry + DEVICE_H / 2.0 - 5);
			cairo_set_line_width(cr, 1.8);
			cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
			cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
			fui_color(ui, 0xffffff00 | alpha);
			cairo_stroke(cr);
		}
		shadowed_text(ui, x + 44, ry + 8, devices[i].name, F_SUB, 0xffffff00 | alpha, FUI_LEFT,
			CONTROL_W - 64);
		if (hover) {
			fui_cursor_pointer(ui);
		}
		if (open > 0.95 && fui_clicked(ui, x + 8, ry, CONTROL_W - 16, DEVICE_H)) {
			if (!devices[i].current) {
				sound_set_device(c->sound, side, devices[i].id);
			}
			c->picking = -1;
			fui_redraw(ui);
		}
	}
}

// the output's or the input's tile, h tall (taller while its devices are
// open in it): its device, the level (a click on the icon mutes), the
// devices behind the chevron
static void level_tile(struct fui *ui, struct control *c, enum sound_side side, double x,
		double y, double h, double open) {
	cairo_t *cr = fui_cairo(ui);
	double volume = 0;
	bool muted = false;
	char name[128];
	bool have = sound_get(c->sound, side, &volume, &muted, name, sizeof(name));
	bool output = side == SOUND_OUTPUT;
	shadowed_text(ui, x + 20, y + 14, output ? "Sound" : "Microphone", F_TITLE, TEXT, FUI_LEFT,
		0);
	shadowed_text(ui, x + CONTROL_W - 20, y + 15, have ? name : "None", F_SUB, TEXT_DIM,
		FUI_RIGHT, CONTROL_W - 150);
	// the icon (mute) at the left, the slider, the devices at the right
	double sy = y + 56, sx = x + 48, sw = CONTROL_W - 104, th = 8;
	uint64_t id = (uint64_t)side;
	if (have && round_button(ui, fui_id("mute", id), x + 26, sy, 15)) {
		sound_set_muted(c->sound, side, !muted);
		muted = !muted;
	}
	bool picking = c->picking == (int)side;
	if (round_button(ui, fui_id("devices", id), x + CONTROL_W - 26, sy, 15)) {
		c->picking = picking ? -1 : (int)side;
		fui_redraw(ui);
	}
	fui_color(ui, have ? TEXT : TEXT_DIM);
	if (output) {
		icon_speaker(cr, x + 26, sy, 18, muted ? -1 : 2);
	} else {
		icon_mic(cr, x + 26, sy, 18, muted);
	}
	// the chevron turns down as it opens
	cairo_save(cr);
	cairo_translate(cr, x + CONTROL_W - 26, sy);
	cairo_rotate(cr, open * M_PI / 2);
	fui_color(ui, TEXT);
	icon_more(cr, 0, 0, 18, false);
	cairo_restore(cr);
	double shown = muted ? 0 : volume;
	fui_fill_rounded(ui, sx, sy - th / 2, sw, th, th / 2, 0xffffff40);
	fui_fill_rounded(ui, sx, sy - th / 2, fmax(sw * shown, th), th, th / 2, TEXT);
	// the devices, in the tile under the slider, as much as it's open
	if (open > 0.01) {
		cairo_save(cr);
		cairo_rectangle(cr, x, y, CONTROL_W, h);
		cairo_clip(cr);
		device_list(ui, c, side, x, y + SOUND_H - 4, open);
		cairo_restore(cr);
	}
	if (!have) {
		return;
	}
	if (fui_hover(ui, sx - 8, sy - 14, sw + 16, 28)) {
		fui_cursor_pointer(ui);
	}
	if (fui_clicked(ui, sx - 8, sy - 14, sw + 16, 28)) {
		c->dragging = (int)side;
	}
	if (c->dragging == (int)side) {
		double px, py;
		fui_pointer(ui, &px, &py);
		double v = fmax(0, fmin((px - sx) / sw, 1));
		if (muted) {
			sound_set_muted(c->sound, side, false);
		}
		if (fabs(v - volume) > 0.004) {
			sound_set_volume(c->sound, side, v);
		}
		c->dragging = fui_button_held(ui) ? (int)side : -1;
		fui_redraw(ui);
	}
}

double control_height(struct control *c) {
	double h = (c->media_open ? MEDIA_H + GAP + TILE_H : 2 * TILE_H + GAP) + GAP + TILE_H
		+ 2 * (GAP + SOUND_H);
	return c->picking >= 0 ? h + list_height(c, (enum sound_side)c->picking) : h;
}

// 0 till a, 1 from b, softly between: a part of an animation that starts
// later or ends sooner than the rest
static double phase(double a, double b, double t) {
	return fui_soft((t - a) / (b - a));
}

// a switch tile, its glass and its insides at x, y
static void switch_at(struct fui *ui, struct control *c, double x, double y,
		struct fui_pane *panes, int *n, int max,
		void (*draw)(struct fui *ui, struct control *c, double x, double y)) {
	if (*n < max) {
		panes[(*n)++] = glass(x, y, HALF, TILE_H, TILE_H / 2.0);
	}
	draw(ui, c, x, y);
}

int control_frame(struct fui *ui, struct control *c, double right, double top,
		struct fui_pane *panes, int max) {
	double x = right - CONTROL_W, y = top;
	// now playing opening, in steps that overlap: bluetooth goes down and
	// over to the right, network down after it once it's clear, then now
	// playing grows into where they were. closing, the same backwards.
	// nothing passes over anything, nothing jumps
	// what's going to grow waits till the surface has grown for it (it's
	// asked for and comes a frame or two later): nothing cut off on the way
	bool room = c->room >= control_height(c) - 0.5;
	// on time, not eased: the steps get their share of it each
	double e = fui_ramp(ui, fui_id("media open", 0), c->media_open && room ? 1 : 0, 0.5);
	double bt_down = phase(0, 0.45, e), bt_across = phase(0.3, 0.6, e);
	double net_down = phase(0.3, 0.75, e), grow = phase(0.45, 1, e);
	double mx = lerp(x + HALF + GAP, x, grow), mw = lerp(HALF, CONTROL_W, grow);
	double mh = lerp(2 * TILE_H + GAP, MEDIA_H, grow);
	double under = y + MEDIA_H + GAP;
	double net_y = lerp(y, under, net_down), bt_y = lerp(y + TILE_H + GAP, under, bt_down);
	double bt_x = lerp(x, x + HALF + GAP, bt_across);
	// each level tile as open as it's going: it grows, what's under it moves.
	// they follow right under the switches
	double open[2], h[2];
	for (int side = 0; side < 2; side++) {
		open[side] = fui_soft(fui_ramp(ui, fui_id("open", (uint64_t)side), c->picking == side
			&& room ? 1 : 0, 0.35));
		h[side] = SOUND_H + open[side] * list_height(c, (enum sound_side)side);
	}
	double quiet_y = fmax(net_y, bt_y) + TILE_H + GAP;
	double out_y = quiet_y + TILE_H + GAP;
	double in_y = out_y + h[SOUND_OUTPUT] + GAP;
	int n = 0;
	struct fui_pane tiles[] = {
		glass(mx, y, mw, mh, 28),
		glass(x, out_y, CONTROL_W, h[SOUND_OUTPUT], 28),
		glass(x, in_y, CONTROL_W, h[SOUND_INPUT], 28),
	};
	for (size_t i = 0; i < sizeof(tiles) / sizeof(tiles[0]) && n < max; i++) {
		panes[n++] = tiles[i];
	}
	switch_at(ui, c, x, net_y, panes, &n, max, network_tile);
	switch_at(ui, c, bt_x, bt_y, panes, &n, max, bluetooth_tile);
	if (n < max) {
		panes[n++] = glass(x, quiet_y, CONTROL_W, TILE_H, TILE_H / 2.0);
	}
	quiet_tile(ui, c, x, quiet_y);
	media_tile(ui, c, mx, y, mw, mh, grow);
	level_tile(ui, c, SOUND_OUTPUT, x, out_y, h[SOUND_OUTPUT], open[SOUND_OUTPUT]);
	level_tile(ui, c, SOUND_INPUT, x, in_y, h[SOUND_INPUT], open[SOUND_INPUT]);
	return n;
}
