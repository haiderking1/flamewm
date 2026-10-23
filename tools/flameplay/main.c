// flameplay: flame's video player, on its glass. the video under the window
// (a layer flame scales), a glass bar over it that fades away while it
// plays: play, fifteen seconds either way, the sound, the time, fullscreen.
// music shows its cover. a file dropped on it plays

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

#include "../flameui/flameui.h"
#include "player.h"
#include "resume.h"
#include "subs.h"

static int W = 960, H = 540;
#define MIN_W 360
#define MIN_H 200
#define PICTURES 6 // frames decoded ahead, at most
#define HIDE_AFTER 2.5 // seconds the bar stays after the pointer stops

#define TEXT 0xffffffeb
#define TEXT_DIM 0xffffff99
#define F_TIME "Inter Medium 9"
#define F_TITLE "Inter Semi-Bold 10.5"

struct app {
	struct fui *ui;
	struct player *player;
	struct fui_layer *layer;
	struct player_picture pictures[PICTURES];
	struct fui_picture *fpics[PICTURES];
	int npics;
	char path[PATH_MAX], title[256], error[256];
	bool shown_any, ticking;
	double vx, vy, vw, vh; // where the video is in the window
	// the pointer: where it was, when it last moved (the bar shows)
	double px, py, moved_at;
	// the scrubber held: where it's dragged to, where and when it last
	// seeked, whether it plays on when let go (it's paused while held)
	bool scrubbing, scrub_resume;
	double scrub_to, scrub_seeked, scrubbed_at;
	double volume, unmuted;
	bool ended_seen;
	bool subs_ticking; // a timer's on to see if the subtitles change
	double pixels; // pixels to a window unit, as last drawn
	// a word on what was just picked (a track), at the top for a moment
	char toast[128];
	double toast_at;
};

static void released(struct fui_picture *picture, void *data) {
	struct app *app = data;
	for (int i = 0; i < app->npics; i++) {
		if (app->fpics[i] == picture && app->player) {
			player_picture_done(app->player, &app->pictures[i]);
		}
	}
}

// ---- showing frames ----

static void present(void *data);

static void present_later(struct fui *ui, void *data) {
	present(data);
}

// the next refresh: a frame's chance to go on screen. before the first's
// there, the layer has no refreshes: a short timer
static void tick(struct app *app) {
	if (app->ticking) {
		return;
	}
	app->ticking = true;
	if (app->shown_any) {
		fui_layer_frame(app->layer, present, app);
	} else {
		fui_after(app->ui, 4, present_later, app);
	}
}

static void present(void *data) {
	struct app *app = data;
	app->ticking = false;
	if (!app->player) {
		return;
	}
	struct player_picture *pic = player_due(app->player);
	if (pic) {
		fui_layer_show(app->layer, pic->data, app->vx, app->vy, app->vw, app->vh);
		app->shown_any = true;
	}
	// played to its end: the window pauses it there, bar up
	if (player_playing(app->player) && player_ended(app->player)) {
		fui_redraw(app->ui);
	}
	// a song's (no picture, or one cover) has nothing to tick for once it's
	// up, a paused video only for the frame a seek's still decoding
	if (player_has_video(app->player) && (player_playing(app->player) || !app->shown_any
			|| player_seeking(app->player))) {
		tick(app);
	}
}

// the engine has something (from its threads): the window's loop looks
static void woken(void *data) {
	struct app *app = data;
	fui_wake(app->ui);
}

// ---- opening ----

// where it's been left and how loud, for next time
static void remember(struct app *app) {
	struct player *p = app->player;
	resume_put_volume(app->volume, app->unmuted);
	if (p) {
		double duration = player_duration(p);
		resume_put(app->path, player_ended(p) ? duration : player_time(p), duration,
			player_has_video(p) && !player_has_cover(p));
	}
}

static void unload(struct app *app) {
	remember(app);
	player_close(app->player); // its threads gone: nothing writes the pictures now
	app->player = NULL;
	for (int i = 0; i < app->npics; i++) {
		fui_picture_destroy(app->fpics[i]);
	}
	app->npics = 0;
	app->shown_any = false;
	app->ended_seen = false;
}

// path opened (what it is, its title), not playing yet: app->error when it
// can't be
static bool open_file(struct app *app, const char *path) {
	unload(app);
	snprintf(app->path, sizeof(app->path), "%s", path);
	app->error[0] = '\0';
	app->player = player_open(path, woken, app, app->error, sizeof(app->error));
	const char *slash = strrchr(path, '/');
	const char *title = app->player ? player_title(app->player) : NULL;
	snprintf(app->title, sizeof(app->title), "%s", title ? title : slash ? slash + 1 : path);
	return app->player != NULL;
}

// the opened file's pictures made, played
static void start(struct app *app) {
	int w, h;
	player_size(app->player, &w, &h);
	// big frames: fewer of them ahead
	int count = (long)w * h > 1920L * 1088 ? 4 : PICTURES;
	if (player_has_video(app->player) && player_gpu(app->player) && app->layer) {
		// decoded on the gpu: its images handed to flame as they are
		struct gpu_image images[PICTURES];
		bool made = player_gpu_images(app->player, count, images), ok = made;
		for (int i = 0; ok && i < count; i++) {
			app->fpics[i] = fui_picture_create_dmabuf(app->layer, images[i].width,
				images[i].height, images[i].fourcc, images[i].modifier, images[i].fd,
				images[i].offset, images[i].stride);
			ok = app->fpics[i] != NULL;
			if (ok) {
				app->pictures[i] = (struct player_picture){.data = app->fpics[i]};
				app->npics++;
			}
		}
		for (int i = 0; made && i < count; i++) {
			close(images[i].fd); // flame has its own now, or doesn't want it
		}
		if (!ok) {
			// flame can't show those: decoded on the cpu after all
			for (int i = 0; i < app->npics; i++) {
				fui_picture_destroy(app->fpics[i]);
			}
			app->npics = 0;
			player_use_software(app->player);
		}
	}
	if (player_has_video(app->player) && !player_gpu(app->player)) {
		for (int i = 0; i < count; i++) {
			app->fpics[i] = app->layer ? fui_picture_create(app->layer, w, h) : NULL;
			if (!app->fpics[i]) {
				break;
			}
			app->pictures[i] = (struct player_picture){.data = app->fpics[i]};
			app->pictures[i].pixels = fui_picture_pixels(app->fpics[i], &app->pictures[i].stride);
			app->npics++;
		}
	}
	if (player_has_video(app->player)) {
		fprintf(stderr, "flameplay: %s: decoded on the %s\n", app->title,
			player_gpu(app->player) ? "gpu (vulkan)" : "cpu");
	}
	// on from where it was left
	double at = resume_get(app->path);
	if (at > 0) {
		player_seek(app->player, at);
	}
	player_set_pictures(app->player, app->pictures, app->npics);
	player_set_volume(app->player, app->volume);
	player_play(app->player);
	tick(app);
}

static bool load(struct app *app, const char *path) {
	if (!open_file(app, path)) {
		return false;
	}
	start(app);
	return true;
}

// ---- the bar ----

static void fmt_time(double t, char *out, size_t size) {
	int s = (int)floor(t + 0.5);
	if (s >= 3600) {
		snprintf(out, size, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
	} else {
		snprintf(out, size, "%d:%02d", s / 60, s % 60);
	}
}

// a round button with a glyph drawn by kind, true when clicked
enum glyph { G_PLAY, G_PAUSE, G_BACK, G_FORWARD, G_SOUND, G_MUTED, G_FULL, G_UNFULL,
	G_TRACKS };

static void glyph(struct fui *ui, enum glyph g, double cx, double cy, double s, uint32_t color) {
	cairo_t *cr = fui_cairo(ui);
	fui_color(ui, color);
	cairo_new_path(cr);
	cairo_set_line_width(cr, 1.8);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	switch (g) {
	case G_PLAY:
		cairo_move_to(cr, cx - s * 0.32, cy - s * 0.42);
		cairo_line_to(cr, cx + s * 0.45, cy);
		cairo_line_to(cr, cx - s * 0.32, cy + s * 0.42);
		cairo_close_path(cr);
		cairo_fill(cr);
		break;
	case G_PAUSE:
		fui_fill_rounded(ui, cx - s * 0.32, cy - s * 0.4, s * 0.22, s * 0.8, s * 0.06, color);
		fui_fill_rounded(ui, cx + s * 0.1, cy - s * 0.4, s * 0.22, s * 0.8, s * 0.06, color);
		break;
	case G_BACK:
	case G_FORWARD: {
		// most of a circle, its arrow at the top going the way it seeks, 15 in it
		double r = s * 0.42, d = g == G_BACK ? -1 : 1;
		double gap = M_PI * 0.32;
		if (g == G_BACK) {
			cairo_arc(cr, cx, cy, r, -M_PI / 2 + gap, -M_PI / 2 + 2 * M_PI);
		} else {
			cairo_arc_negative(cr, cx, cy, r, -M_PI / 2 - gap, -M_PI / 2 - 2 * M_PI);
		}
		cairo_stroke(cr);
		double ax = cx, ay = cy - r;
		cairo_move_to(cr, ax - d * s * 0.16, ay - s * 0.14);
		cairo_line_to(cr, ax, ay);
		cairo_line_to(cr, ax - d * s * 0.16, ay + s * 0.14);
		cairo_stroke(cr);
		fui_text(ui, cx, cy - s * 0.2, "15", "Inter Bold 6.5", color, FUI_CENTER, 0);
		break;
	}
	case G_SOUND:
	case G_MUTED:
		cairo_move_to(cr, cx - s * 0.45, cy - s * 0.15);
		cairo_line_to(cr, cx - s * 0.25, cy - s * 0.15);
		cairo_line_to(cr, cx, cy - s * 0.4);
		cairo_line_to(cr, cx, cy + s * 0.4);
		cairo_line_to(cr, cx - s * 0.25, cy + s * 0.15);
		cairo_line_to(cr, cx - s * 0.45, cy + s * 0.15);
		cairo_close_path(cr);
		cairo_fill(cr);
		if (g == G_SOUND) {
			cairo_arc(cr, cx + s * 0.05, cy, s * 0.22, -M_PI / 4, M_PI / 4);
			cairo_stroke(cr);
			cairo_arc(cr, cx + s * 0.05, cy, s * 0.4, -M_PI / 4, M_PI / 4);
			cairo_stroke(cr);
		} else {
			cairo_move_to(cr, cx + s * 0.15, cy - s * 0.15);
			cairo_line_to(cr, cx + s * 0.45, cy + s * 0.15);
			cairo_move_to(cr, cx + s * 0.45, cy - s * 0.15);
			cairo_line_to(cr, cx + s * 0.15, cy + s * 0.15);
			cairo_stroke(cr);
		}
		break;
	case G_TRACKS: {
		// a speech bubble, two lines of words in it
		double w = s * 0.9, h = s * 0.66, x = cx - w / 2, y = cy - h / 2 - s * 0.06;
		fui_rounded(ui, x, y, w, h, s * 0.16);
		cairo_stroke(cr);
		cairo_move_to(cr, x + w * 0.24, y + h);
		cairo_line_to(cr, x + w * 0.2, y + h + s * 0.18);
		cairo_line_to(cr, x + w * 0.44, y + h);
		cairo_stroke(cr);
		cairo_move_to(cr, x + w * 0.24, y + h * 0.4);
		cairo_line_to(cr, x + w * 0.76, y + h * 0.4);
		cairo_move_to(cr, x + w * 0.24, y + h * 0.66);
		cairo_line_to(cr, x + w * 0.58, y + h * 0.66);
		cairo_stroke(cr);
		break;
	}
	case G_FULL:
	case G_UNFULL: {
		// four corners: pointing out to go fullscreen, in to come back
		double o = s * 0.42, l = s * 0.24;
		for (int i = 0; i < 4; i++) {
			double sx = i % 2 ? 1 : -1, sy = i / 2 ? 1 : -1;
			double x = cx + sx * (g == G_FULL ? o : o - l), y = cy + sy * (g == G_FULL ? o : o - l);
			double dx = g == G_FULL ? -sx * l : sx * l, dy = g == G_FULL ? -sy * l : sy * l;
			cairo_move_to(cr, x + dx, y);
			cairo_line_to(cr, x, y);
			cairo_line_to(cr, x, y + dy);
		}
		cairo_stroke(cr);
		break;
	}
	}
}

static bool button(struct fui *ui, uint64_t id, enum glyph g, double cx, double cy, double s) {
	double r = s * 0.9;
	bool hover = fui_hover(ui, cx - r, cy - r, 2 * r, 2 * r);
	double lit = fui_ease(ui, id, hover ? 1 : 0, 20);
	if (lit > 0.01) {
		cairo_arc(fui_cairo(ui), cx, cy, r, 0, 2 * M_PI);
		fui_color(ui, (uint32_t)(0xffffff00 | (uint32_t)(0x1c * lit)));
		cairo_fill(fui_cairo(ui));
	}
	if (hover) {
		fui_cursor_pointer(ui);
	}
	glyph(ui, g, cx, cy, s, TEXT);
	return fui_clicked(ui, cx - r, cy - r, 2 * r, 2 * r);
}

static void toggle(struct app *app) {
	if (player_playing(app->player)) {
		player_pause(app->player);
		remember(app);
	} else {
		player_play(app->player);
		app->ended_seen = false;
		tick(app);
	}
}

static void seek_by(struct app *app, double by) {
	player_seek(app->player, player_time(app->player) + by);
	tick(app);
}

static void set_volume(struct app *app, double v) {
	app->volume = fmax(0, fmin(v, 1));
	player_set_volume(app->player, app->volume);
}

static void say(struct app *app, const char *what) {
	snprintf(app->toast, sizeof(app->toast), "%s", what);
	app->toast_at = fui_time(app->ui);
	fui_redraw(app->ui);
}

static bool has_tracks(struct app *app) {
	struct player_track t[2];
	return player_tracks(app->player, PLAYER_AUDIO, t, 2) > 1
		|| player_tracks(app->player, PLAYER_SUBTITLES, t, 1) > 0;
}

// the next track of kind on (subtitles: after the last, none), said
static void cycle(struct app *app, enum player_track_kind kind) {
	struct player_track tracks[32];
	int n = player_tracks(app->player, kind, tracks, 32), on = -1;
	for (int i = 0; i < n; i++) {
		on = tracks[i].on ? i : on;
	}
	bool audio = kind == PLAYER_AUDIO;
	if (n == 0 || (audio && n < 2)) {
		say(app, audio ? "Only one sound" : "No subtitles");
		return;
	}
	int next = on + 1 < n ? on + 1 : audio ? 0 : -1;
	player_set_track(app->player, kind, next);
	char what[160];
	snprintf(what, sizeof(what), "%s: %s", audio ? "Audio" : "Subtitles",
		next >= 0 ? tracks[next].name : "Off");
	say(app, what);
	tick(app);
}

// the bar: bx, by, bw x bh, faded to shown
static void bar(struct app *app, double bx, double by, double bw, double bh) {
	struct fui *ui = app->ui;
	struct player *p = app->player;
	double cy = by + 24;
	// the middle: back, play, forward
	double mid = bx + bw / 2;
	if (button(ui, fui_id("back", 0), G_BACK, mid - 52, cy, 20)) {
		seek_by(app, -15);
	}
	if (button(ui, fui_id("play", 0), player_playing(p) ? G_PAUSE : G_PLAY, mid, cy, 24)) {
		toggle(app);
	}
	if (button(ui, fui_id("forward", 0), G_FORWARD, mid + 52, cy, 20)) {
		seek_by(app, 15);
	}
	// the left: the sound
	if (button(ui, fui_id("mute", 0), app->volume > 0 ? G_SOUND : G_MUTED, bx + 26, cy, 18)) {
		if (app->volume > 0) {
			app->unmuted = app->volume;
			set_volume(app, 0);
		} else {
			set_volume(app, app->unmuted > 0 ? app->unmuted : 1);
		}
	}
	if (bw > 420) {
		double v = app->volume;
		if (fui_slider(ui, fui_id("volume", 0), bx + 46, cy - 11, 86, &v, 0, 1, 0.01, true)
				|| v != app->volume) {
			set_volume(app, v);
		}
	}
	// the right: the sounds and subtitles to pick from, fullscreen
	if (has_tracks(app)
			&& button(ui, fui_id("tracks", 0), G_TRACKS, bx + bw - 64, cy, 18)) {
		fui_menu_open(ui, fui_id("tracks", 1), bx + bw - 64, cy - 14);
	}
	bool full = fui_fullscreen(ui);
	if (button(ui, fui_id("full", 0), full ? G_UNFULL : G_FULL, bx + bw - 26, cy, 16)) {
		fui_set_fullscreen(ui, !full);
	}
	// under them: the time, the scrubber
	double ty = by + bh - 22, duration = player_duration(p);
	double t = app->scrubbing ? app->scrub_to : player_time(p);
	char left[32], right[32];
	fmt_time(t, left, sizeof(left));
	right[0] = '-';
	fmt_time(fmax(duration - t, 0), right + 1, sizeof(right) - 1);
	double lw = fui_text_width(ui, duration >= 3600 ? "0:00:00" : "00:00", F_TIME) + 4;
	fui_text(ui, bx + 16, ty - 6, left, F_TIME, TEXT_DIM, FUI_LEFT, 0);
	fui_text(ui, bx + bw - 16, ty - 6, right, F_TIME, TEXT_DIM, FUI_RIGHT, 0);
	double sx = bx + 16 + lw + 8, sw = bw - 2 * (16 + lw + 8);
	bool over = fui_hover(ui, sx - 6, ty - 8, sw + 12, 16);
	double grow = fui_ease(ui, fui_id("scrub", 1), over || app->scrubbing ? 1 : 0, 18);
	double th = 4 + 2 * grow;
	fui_fill_rounded(ui, sx, ty - th / 2, sw, th, th / 2, 0xffffff33);
	double frac = duration > 0 ? fmin(t / duration, 1) : 0;
	fui_fill_rounded(ui, sx, ty - th / 2, fmax(sw * frac, th), th, th / 2, 0xffffffe6);
	if (grow > 0.01) {
		cairo_arc(fui_cairo(ui), sx + sw * frac, ty, 6 * grow, 0, 2 * M_PI);
		fui_color(ui, 0xffffffff);
		cairo_fill(fui_cairo(ui));
	}
	if (over) {
		fui_cursor_pointer(ui);
	}
	double px, py;
	fui_pointer(ui, &px, &py);
	if (duration > 0 && fui_clicked(ui, sx - 6, ty - 8, sw + 12, 16)) {
		// silent while it's held: only frames, no bits of sound each seek
		app->scrubbing = true;
		app->scrub_resume = player_playing(p);
		app->scrub_seeked = -1;
		player_pause(p);
	}
	if (app->scrubbing) {
		app->scrub_to = fmax(0, fmin((px - sx) / sw, 1)) * duration;
		double now = fui_time(ui);
		bool held = fui_button_held(ui);
		// frames as it's dragged, and where it's let go (not again if it's
		// there already)
		if (app->scrub_to != app->scrub_seeked && (!held || now - app->scrubbed_at > 0.08)) {
			app->scrubbed_at = now;
			app->scrub_seeked = app->scrub_to;
			player_seek(p, app->scrub_to);
			tick(app);
		}
		if (!held) {
			app->scrubbing = false;
			if (app->scrub_resume) {
				toggle(app);
			}
		}
	}
}

// ---- keys, drops ----

static void keys(struct app *app) {
	struct fui *ui = app->ui;
	struct player *p = app->player;
	// one by one as they went down: two downs are two steps, and whatever
	// came before q is done before it quits
	uint32_t sym;
	unsigned mods;
	const char *text;
	for (int i = 0; fui_key_at(ui, i, &sym, &mods, &text); i++) {
		sym = xkb_keysym_to_lower(sym);
		if (mods == FUI_CTRL && sym == XKB_KEY_w) {
			fui_quit(ui);
			return;
		}
		if (mods) {
			continue;
		}
		switch (sym) {
		case XKB_KEY_space:
		case XKB_KEY_k:
			toggle(app);
			break;
		case XKB_KEY_Left:
			seek_by(app, -5);
			break;
		case XKB_KEY_Right:
			seek_by(app, 5);
			break;
		case XKB_KEY_j:
			seek_by(app, -15);
			break;
		case XKB_KEY_l:
			seek_by(app, 15);
			break;
		case XKB_KEY_Home:
			player_seek(p, 0);
			tick(app);
			break;
		case XKB_KEY_Up:
			set_volume(app, app->volume + 0.05);
			break;
		case XKB_KEY_Down:
			set_volume(app, app->volume - 0.05);
			break;
		case XKB_KEY_m:
			if (app->volume > 0) {
				app->unmuted = app->volume;
				set_volume(app, 0);
			} else {
				set_volume(app, app->unmuted > 0 ? app->unmuted : 1);
			}
			break;
		case XKB_KEY_f:
			fui_set_fullscreen(ui, !fui_fullscreen(ui));
			break;
		case XKB_KEY_s:
			cycle(app, PLAYER_SUBTITLES);
			break;
		case XKB_KEY_a:
			cycle(app, PLAYER_AUDIO);
			break;
		case XKB_KEY_Escape:
			if (fui_fullscreen(ui)) {
				fui_set_fullscreen(ui, false);
			}
			break;
		case XKB_KEY_q:
			fui_quit(ui);
			return;
		}
	}
}

// a file dropped on it: that plays now
static void drops(struct app *app) {
	struct fui *ui = app->ui;
	double x, y;
	if (fui_dnd_over(ui, &x, &y)) {
		fui_dnd_accept(ui, FUI_DND_COPY);
	}
	size_t len;
	enum fui_dnd_action action;
	char **paths = fui_dnd_dropped(ui, &len, &action, &x, &y);
	if (paths && len) {
		load(app, paths[0]);
	}
}

// ---- the window ----

static void redraw_later(struct fui *ui, void *data) {
	fui_redraw(ui);
}

// the sounds and subtitles to pick from, when it's open
static void tracks_menu(struct app *app) {
	struct fui *ui = app->ui;
	uint64_t id = fui_id("tracks", 1);
	if (!fui_menu_is_open(ui, id)) {
		return;
	}
	struct player_track audio[32], subtitles[32];
	int na = player_tracks(app->player, PLAYER_AUDIO, audio, 32);
	int ns = player_tracks(app->player, PLAYER_SUBTITLES, subtitles, 32);
	struct fui_menu_item items[70];
	int kind[70], which[70], n = 0;
	bool any_sub = false;
	if (na > 1) {
		items[n] = (struct fui_menu_item){.label = "Audio", .heading = true};
		kind[n] = which[n] = -1;
		n++;
		for (int i = 0; i < na; i++, n++) {
			items[n] = (struct fui_menu_item){.label = audio[i].name, .checked = audio[i].on};
			kind[n] = PLAYER_AUDIO;
			which[n] = i;
		}
	}
	if (ns > 0) {
		if (n) {
			items[n] = (struct fui_menu_item){0};
			kind[n] = which[n] = -1;
			n++;
		}
		items[n] = (struct fui_menu_item){.label = "Subtitles", .heading = true};
		kind[n] = which[n] = -1;
		n++;
		int off = n++;
		for (int i = 0; i < ns; i++, n++) {
			items[n] = (struct fui_menu_item){.label = subtitles[i].name, .checked = subtitles[i].on};
			kind[n] = PLAYER_SUBTITLES;
			which[n] = i;
			any_sub |= subtitles[i].on;
		}
		items[off] = (struct fui_menu_item){.label = "Off", .checked = !any_sub};
		kind[off] = PLAYER_SUBTITLES;
		which[off] = -1;
	}
	int picked = fui_menu(ui, id, items, n);
	if (picked >= 0 && kind[picked] >= 0) {
		player_set_track(app->player, (enum player_track_kind)kind[picked], which[picked]);
		tick(app);
	}
}

static void subs_check(struct fui *ui, void *data);

// the subtitles at t over the video, lifted over the bar while it's up:
// false when none are on screen
static bool draw_subtitles(struct app *app, double bar_top, double shown) {
	struct fui *ui = app->ui;
	struct subs *subs = player_subs(app->player);
	if (!subs || !player_has_video(app->player)) {
		return false;
	}
	cairo_t *cr = fui_cairo(ui);
	double sx = 1, sy = 1;
	cairo_user_to_device_distance(cr, &sx, &sy);
	app->pixels = sx;
	subs_update(subs, player_time(app->player), (int)lround(app->vw * sx),
		(int)lround(app->vh * sy));
	// the lowest line's a little over the video's bottom
	double low = app->vy + app->vh - app->vh * 0.05;
	double lift = shown * fmax(0, low - bar_top + 6);
	bool drawn = subs_draw(subs, cr, app->vx, app->vy, lift);
	// while it plays: looked at again shortly, drawn when they've changed
	if (player_playing(app->player) && !app->subs_ticking) {
		app->subs_ticking = true;
		fui_after(ui, 40, subs_check, app);
	}
	return drawn;
}

static void subs_check(struct fui *ui, void *data) {
	struct app *app = data;
	app->subs_ticking = false;
	struct subs *subs = app->player ? player_subs(app->player) : NULL;
	if (!subs || !player_playing(app->player)) {
		return;
	}
	if (subs_update(subs, player_time(app->player), (int)lround(app->vw * app->pixels),
			(int)lround(app->vh * app->pixels))) {
		fui_redraw(ui);
	} else {
		app->subs_ticking = true;
		fui_after(ui, 40, subs_check, app);
	}
}

// what was just picked, in a pill at the top, going after a moment: false
// when it's gone
static bool draw_toast(struct app *app) {
	struct fui *ui = app->ui;
	double age = fui_time(ui) - app->toast_at;
	if (!app->toast[0] || age > 1.8) {
		return false;
	}
	double a = age < 1.5 ? 1 : 1 - (age - 1.5) / 0.3;
	double tw = fui_text_width(ui, app->toast, F_TITLE) + 32, th = 30;
	double x = (W - tw) / 2, y = 44;
	cairo_t *cr = fui_cairo(ui);
	cairo_push_group(cr);
	fui_fill_rounded(ui, x, y, tw, th, th / 2, 0x1a1a20d0);
	fui_text(ui, W / 2.0, y + 7, app->toast, F_TITLE, TEXT, FUI_CENTER, 0);
	cairo_pop_group_to_source(cr);
	cairo_paint_with_alpha(cr, a);
	fui_after(ui, 30, redraw_later, app);
	return true;
}

static void frame(struct fui *ui, void *data) {
	struct app *app = data;
	W = fui_width(ui);
	H = fui_height(ui);
	cairo_t *cr = fui_cairo(ui);
	bool full = fui_fullscreen(ui);
	double radius = full ? 0 : 12;

	if (!app->player) {
		fui_fill_rounded(ui, 0, 0, W, H, radius, 0x161619ff);
		fui_text(ui, W / 2.0, H / 2.0 - 18, app->path[0] ? "Can't play it" : "Drop a video here",
			F_TITLE, TEXT, FUI_CENTER, W - 40);
		fui_text(ui, W / 2.0, H / 2.0 + 4, app->error[0] ? app->error : app->title, F_TIME,
			TEXT_DIM, FUI_CENTER, W - 40);
		fui_set_glass(ui, NULL, 0);
		drops(app);
		return;
	}
	struct player *p = app->player;
	keys(app);
	drops(app);

	// the video as big as it fits, black round it
	double aspect = player_aspect(p);
	app->vw = W;
	app->vh = W / aspect;
	if (app->vh > H) {
		app->vh = H;
		app->vw = H * aspect;
	}
	app->vx = (W - app->vw) / 2;
	app->vy = (H - app->vh) / 2;
	if (app->shown_any) {
		fui_layer_place(app->layer, app->vx, app->vy, app->vw, app->vh);
	}
	fui_rounded(ui, 0, 0, W, H, radius);
	cairo_rectangle(cr, app->vx, app->vy, app->vw, app->vh);
	cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
	fui_color(ui, 0x000000ff);
	cairo_fill(cr);
	cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
	if (!player_has_video(p)) {
		// a song without a cover: its name, big
		fui_fill_rounded(ui, 0, 0, W, H, radius, 0x161619ff);
		fui_text(ui, W / 2.0, H / 2.0 - 40, "♪", "Inter 40", TEXT_DIM, FUI_CENTER, 0);
		fui_text(ui, W / 2.0, H / 2.0 + 14, app->title, F_TITLE, TEXT, FUI_CENTER, W - 40);
	}

	// the end: paused there, the bar up
	if (player_playing(p) && player_ended(p)) {
		player_pause(p);
		app->ended_seen = true;
		remember(app); // watched: next time from the start
	}

	// the bar shows while the pointer moves, over it, paused
	double px, py, now = fui_time(ui);
	fui_pointer(ui, &px, &py);
	if (px != app->px || py != app->py) {
		app->px = px;
		app->py = py;
		app->moved_at = now;
	}
	double bw = fmin(W - 32, 620), bh = 72, bx = (W - bw) / 2, by = H - bh - 16;
	bool over_bar = fui_hover(ui, bx, by, bw, bh);
	bool show = !player_playing(p) || over_bar || app->scrubbing
		|| now - app->moved_at < HIDE_AFTER || !player_has_video(p)
		|| fui_menu_is_open(ui, fui_id("tracks", 1));
	double shown = fui_ease(ui, fui_id("bar", 0), show ? 1 : 0, 10);
	bool words = draw_subtitles(app, by, shown);
	if (show && player_playing(p)) {
		// to hide it once the pointer's been still long enough, and to
		// keep the time going
		fui_after(ui, 250, redraw_later, app);
	}
	if (shown > 0.002) {
		// the glass fades with what's on it, all of it at once: no frosted
		// patch left a moment, no pop
		struct fui_pane pane = {(int)bx, (int)by, (int)bw, (int)bh, 18, 26, 0x1a1a20a0, 1.6,
			0.45, 0};
		fui_set_glass(ui, &pane, 1);
		fui_set_glass_opacity(ui, shown);
		cairo_push_group(cr);
		// the title at the top, over a shade
		cairo_pattern_t *shade = cairo_pattern_create_linear(0, 0, 0, 56);
		cairo_pattern_add_color_stop_rgba(shade, 0, 0, 0, 0, 0.55);
		cairo_pattern_add_color_stop_rgba(shade, 1, 0, 0, 0, 0);
		cairo_save(cr);
		fui_rounded(ui, 0, 0, W, H, radius);
		cairo_clip(cr);
		cairo_rectangle(cr, 0, 0, W, 56);
		cairo_set_source(cr, shade);
		cairo_fill(cr);
		cairo_restore(cr);
		cairo_pattern_destroy(shade);
		fui_text(ui, W / 2.0, 12, app->title, F_TITLE, TEXT, FUI_CENTER, W - 80);
		bar(app, bx, by, bw, bh);
		cairo_pop_group_to_source(cr);
		cairo_paint_with_alpha(cr, shown);
	} else {
		fui_set_glass(ui, NULL, 0);
	}
	bool toast = draw_toast(app);
	tracks_menu(app);
	// watching it fill the screen, nothing over it: the window shows
	// nothing, the video goes to the screen as it is (no drawing by flame)
	bool fills = app->vx < 0.5 && app->vy < 0.5 && app->vw > W - 0.5 && app->vh > H - 0.5;
	if (full && fills && shown <= 0.002 && !words && !toast
			&& !fui_menu_is_open(ui, fui_id("tracks", 1))) {
		fui_nothing(ui);
	}
	// watching: the pointer goes as the bar starts going, not after
	if (!show && player_playing(p) && fui_hover(ui, 0, 0, W, H)) {
		fui_cursor_hide(ui);
	}
	// a click on the video plays or pauses it, a double click goes fullscreen
	if (fui_clicked(ui, 0, 0, W, H)) {
		if (fui_click_count(ui) == 2) {
			toggle(app); // the first click's undone
			fui_set_fullscreen(ui, !full);
		} else {
			toggle(app);
		}
	}
	if (!full) {
		fui_rounded(ui, 0.5, 0.5, W - 1, H - 1, radius);
		fui_color(ui, 0xffffff1c);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
	}
}

int main(int argc, char **argv) {
	struct app app = {.volume = 1, .unmuted = 1};
	resume_volume(&app.volume, &app.unmuted);
	const char *path = argc > 1 ? argv[1] : NULL;
	// opened before the window: its first size is the video's shape, at
	// most 1280 x 720 (it plays once the window's up)
	if (path && open_file(&app, path)) {
		if (player_has_video(app.player)) {
			double aspect = player_aspect(app.player);
			int w, h;
			player_size(app.player, &w, &h);
			W = (int)fmin(fmax(w, 640), 1280);
			H = (int)(W / aspect);
			if (H > 720) {
				H = 720;
				W = (int)(H * aspect);
			}
		} else {
			W = 480;
			H = 270;
		}
	}
	app.ui = fui_create(&(struct fui_config){
		.app_id = "flameplay",
		.title = "Player",
		.width = W,
		.height = H,
		.min_width = MIN_W,
		.min_height = MIN_H,
		.corner_radius = 12,
		.frame = frame,
		.data = &app,
	});
	if (!app.ui) {
		fprintf(stderr, "flameplay: no wayland display\n");
		return 1;
	}
	app.layer = fui_layer_create(app.ui);
	if (!app.layer) {
		fprintf(stderr, "flameplay: the compositor can't show a video layer\n");
	} else {
		fui_layer_on_release(app.layer, released, &app);
	}
	if (app.player) {
		start(&app);
	} else if (path) {
		fprintf(stderr, "flameplay: %s: %s\n", path, app.error);
	}
	int status = fui_run(app.ui);
	unload(&app);
	fui_layer_destroy(app.layer);
	fui_destroy(app.ui);
	return status;
}
