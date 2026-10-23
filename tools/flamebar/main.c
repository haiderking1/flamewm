// flamebar: flame's bar across the top of the screen, see through: the
// app that has the keys, the workspaces in use, the control center's
// button and the time, over the wallpaper. what's where comes from flame's
// ipc (subscribe), every change as it happens. the control center opens
// under its button: the bar's surface grows for it, a click anywhere else
// closes it

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../flameipc/ipc.h"
#include "../flameipc/json.h"
#include "../flameui/flameui.h"
#include "bus.h"
#include "control.h"
#include "notes.h"
#include "osd.h"

#define BAR_H 32
#define F_APP "Inter Semi-Bold 10.5"
#define F_BAR "Inter Medium 10.5"

#define WORKSPACES 10

struct app {
	struct fui *ui;
	// flame's ipc, subscribed: a state line every change
	int fd;
	char line[65536];
	size_t have;
	// what it said last
	char app_name[128];
	int workspace;
	bool used[WORKSPACES + 1];
	// what's open under the bar: the control center or the notifications
	enum { OPEN_NONE, OPEN_CONTROL, OPEN_LIST } open;
	int popped; // how tall the surface was grown for it (open, or closing), 0: not
	int grown; // how tall it was grown for cards with nothing open, 0: not
	struct control control;
	struct notes notes;
	struct osd osd; // what a media key just changed
};

static void woken(void *data) {
	struct app *app = data;
	fui_wake(app->ui);
}

static void changed(void *data) {
	struct app *app = data;
	fui_redraw(app->ui);
}

// ---- flame ----

static void connect_later(struct fui *ui, void *data);

// an app's name to show: flame's own by theirs, the last part of a
// reverse-dns id, capitalized
static void pretty_name(const char *app_id, char *out, size_t size) {
	static const char *const own[][2] = {
		{"flamefiles", "Files"}, {"flameplay", "Player"}, {"flamesettings", "Settings"},
	};
	for (size_t i = 0; i < sizeof(own) / sizeof(own[0]); i++) {
		if (!strcmp(app_id, own[i][0])) {
			snprintf(out, size, "%s", own[i][1]);
			return;
		}
	}
	const char *dot = strrchr(app_id, '.');
	snprintf(out, size, "%s", dot && dot[1] ? dot + 1 : app_id);
	if (out[0] >= 'a' && out[0] <= 'z') {
		out[0] = (char)(out[0] - 'a' + 'A');
	}
}

static double now_seconds(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

// a sixteenth up or down, on the sixteenths: the steps a key takes
static double step(double level, int by) {
	return fmax(0, fmin(round(level * 16 + by) / 16, 1));
}

// a media key flame sent: changed, and shown for a moment (not while the
// control center's open: its sliders show it)
static void media_key(struct app *app, const char *key) {
	struct control *c = &app->control;
	double volume, now = now_seconds();
	bool muted;
	char name[128];
	bool up = !strcmp(key, "volume-up");
	if (up || !strcmp(key, "volume-down")) {
		if (!sound_get(c->sound, SOUND_OUTPUT, &volume, &muted, name, sizeof(name))) {
			return;
		}
		volume = step(volume, up ? 1 : -1);
		sound_set_volume(c->sound, SOUND_OUTPUT, volume);
		if (up && muted) {
			sound_set_muted(c->sound, SOUND_OUTPUT, false); // up's heard
			muted = false;
		}
		osd_show(&app->osd, OSD_VOLUME, volume, muted, now);
	} else if (!strcmp(key, "mute")) {
		if (sound_get(c->sound, SOUND_OUTPUT, &volume, &muted, name, sizeof(name))) {
			sound_set_muted(c->sound, SOUND_OUTPUT, !muted);
			osd_show(&app->osd, OSD_VOLUME, volume, !muted, now);
		}
	} else if (!strcmp(key, "mic-mute")) {
		if (sound_get(c->sound, SOUND_INPUT, &volume, &muted, name, sizeof(name))) {
			sound_set_muted(c->sound, SOUND_INPUT, !muted);
			osd_show(&app->osd, OSD_MIC, volume, !muted, now);
		}
	} else if (!strncmp(key, "brightness-", 11)) {
		if (backlight_there(c->backlight)) {
			double level = step(backlight_get(c->backlight), !strcmp(key, "brightness-up")
				? 1 : -1);
			backlight_set(c->backlight, level);
			osd_show(&app->osd, OSD_BRIGHTNESS, level, false, now);
		}
	} else if (!strcmp(key, "play-pause")) {
		media_control(c->media, "PlayPause");
	} else if (!strcmp(key, "next")) {
		media_control(c->media, "Next");
	} else if (!strcmp(key, "previous")) {
		media_control(c->media, "Previous");
	}
	fui_redraw(app->ui);
}

static void took_state(struct app *app, const char *text) {
	struct json *state = json_parse(text);
	if (state && !strcmp(json_str(json_get(state, "event"), ""), "key")) {
		media_key(app, json_str(json_get(state, "key"), ""));
		json_free(state);
		return;
	}
	if (!state || strcmp(json_str(json_get(state, "event"), ""), "state")) {
		json_free(state);
		return;
	}
	const struct json *window = json_get(state, "window");
	const char *id = json_str(json_get(window, "app_id"), "");
	if (*id) {
		pretty_name(id, app->app_name, sizeof(app->app_name));
	} else {
		snprintf(app->app_name, sizeof(app->app_name), "Flame");
	}
	app->workspace = (int)json_num(json_get(state, "workspace"), 1);
	memset(app->used, 0, sizeof(app->used));
	const struct json *list = json_get(state, "workspaces");
	for (size_t i = 0; list && i < list->len; i++) {
		const struct json *w = json_at(list, i);
		int n = (int)json_num(json_get(w, "id"), 0);
		if (n >= 1 && n <= WORKSPACES) {
			app->used[n] = json_num(json_get(w, "windows"), 0) > 0;
		}
	}
	json_free(state);
	fui_redraw(app->ui);
}

static void readable(struct fui *ui, int fd, void *data) {
	struct app *app = data;
	ssize_t got = read(fd, app->line + app->have, sizeof(app->line) - 1 - app->have);
	if (got <= 0) {
		// flame's gone (restarting): again in a bit
		fui_unwatch(ui, fd);
		close(fd);
		app->fd = -1;
		app->have = 0;
		fui_after(ui, 1000, connect_later, app);
		return;
	}
	app->have += (size_t)got;
	app->line[app->have] = '\0';
	// every whole line: the last one says how it is now
	char *start = app->line, *nl;
	while ((nl = strchr(start, '\n'))) {
		*nl = '\0';
		took_state(app, start);
		start = nl + 1;
	}
	app->have = strlen(start);
	memmove(app->line, start, app->have + 1);
	if (app->have == sizeof(app->line) - 1) {
		app->have = 0; // a line this long isn't one of flame's
	}
}

static void connect_later(struct fui *ui, void *data) {
	struct app *app = data;
	app->fd = ipc_connect();
	// what's where as it changes, and the media keys
	static const char ask[] = "subscribe\nmedia-keys\n";
	if (app->fd < 0 || write(app->fd, ask, sizeof(ask) - 1) != (ssize_t)sizeof(ask) - 1
			|| !fui_watch(ui, app->fd, readable, app)) {
		if (app->fd >= 0) {
			close(app->fd);
			app->fd = -1;
		}
		fui_after(ui, 1000, connect_later, app);
	}
}

static void workspace(int n) {
	char command[32];
	snprintf(command, sizeof(command), "workspace %d", n);
	free(ipc_call(command));
}

// ---- drawing ----

// flame's mark: a flame, its tip leaning
static void flame_mark(struct fui *ui, double cx, double cy, double s) {
	cairo_t *cr = fui_cairo(ui);
	cairo_new_path(cr);
	cairo_move_to(cr, cx + 0.08 * s, cy - 0.5 * s);
	cairo_curve_to(cr, cx + 0.45 * s, cy - 0.18 * s, cx + 0.42 * s, cy + 0.12 * s,
		cx + 0.3 * s, cy + 0.32 * s);
	cairo_curve_to(cr, cx + 0.18 * s, cy + 0.52 * s, cx - 0.18 * s, cy + 0.52 * s,
		cx - 0.3 * s, cy + 0.32 * s);
	cairo_curve_to(cr, cx - 0.42 * s, cy + 0.12 * s, cx - 0.38 * s, cy - 0.12 * s,
		cx - 0.2 * s, cy - 0.22 * s);
	cairo_curve_to(cr, cx - 0.2 * s, cy - 0.05 * s, cx - 0.12 * s, cy + 0.02 * s,
		cx - 0.05 * s, cy + 0.02 * s);
	cairo_curve_to(cr, cx - 0.1 * s, cy - 0.2 * s, cx - 0.02 * s, cy - 0.38 * s,
		cx + 0.08 * s, cy - 0.5 * s);
	cairo_close_path(cr);
	cairo_save(cr);
	cairo_translate(cr, 0, 0.6);
	fui_color(ui, 0x00000040);
	cairo_fill_preserve(cr);
	cairo_restore(cr);
	fui_color(ui, TEXT);
	cairo_fill(cr);
}

// the control center's mark: two switches, one on
static void switches_mark(struct fui *ui, double cx, double cy, uint32_t color) {
	cairo_t *cr = fui_cairo(ui);
	for (int i = 0; i < 2; i++) {
		double y = cy + (i ? 3.2 : -3.2), knob = cx + (i ? 3.2 : -3.2);
		fui_rounded(ui, cx - 7, y - 2.6, 14, 5.2, 2.6);
		fui_color(ui, color);
		cairo_set_line_width(cr, 1.3);
		cairo_stroke(cr);
		cairo_arc(cr, knob, y, 1.6, 0, 2 * M_PI);
		cairo_fill(cr);
	}
}

// how wide the time is at most today, whatever minute it is: its digits
// as the widest digit (they're not all as wide as each other)
static double clock_room(struct fui *ui, const char *when) {
	static char widest;
	if (!widest) {
		double most = 0;
		for (char d = '0'; d <= '9'; d++) {
			double tw = fui_text_width(ui, (char[]){d, '\0'}, F_BAR);
			if (tw > most) {
				most = tw;
				widest = d;
			}
		}
	}
	char text[64];
	snprintf(text, sizeof(text), "%s", when);
	char *colon = strrchr(text, ':');
	for (char *c = colon ? colon - 2 : text; c && *c && colon; c++) {
		if (*c >= '0' && *c <= '9') {
			*c = widest;
		}
	}
	return fui_text_width(ui, text, F_BAR);
}

static void clock_tick(struct fui *ui, void *data) {
	fui_redraw(ui);
}

#define LIST_MAX 600 // the notifications list's tallest, it scrolls past that

// what's in a popover: the control center, or the notifications list
struct popover_draw {
	struct app *app;
	bool control;
	double list_h;
};

// a popover under the bar at right, top, faded in by shown: as one, a
// group the size of it (not of the screen), straight in once it's all there
static void popover(struct fui *ui, double right, double top, double shown,
		struct popover_draw what, struct fui_pane *panes, int *n) {
	cairo_t *cr = fui_cairo(ui);
	bool fading = shown < 1;
	cairo_save(cr);
	cairo_rectangle(cr, right - CONTROL_W - 14, top - 2, CONTROL_W + 16,
		fui_height(ui) - top + 2);
	cairo_clip(cr);
	if (fading) {
		cairo_push_group(cr);
	}
	if (what.control) {
		struct control *c = &what.app->control;
		c->room = fui_height(ui) - top - 16;
		*n += control_frame(ui, c, right, top, panes + *n, 16 - *n);
	} else {
		notes_list(ui, &what.app->notes, right, top, what.list_h, panes, n, 16);
	}
	if (fading) {
		cairo_pop_group_to_source(cr);
		cairo_paint_with_alpha(cr, shown);
	}
	cairo_restore(cr);
}

static void frame(struct fui *ui, void *data) {
	struct app *app = data;
	int w = fui_width(ui);
	cairo_t *cr = fui_cairo(ui);
	double cy = BAR_H / 2.0;

	// the faintest shade: white text reads on a light wallpaper too
	cairo_pattern_t *shade = cairo_pattern_create_linear(0, 0, 0, BAR_H);
	cairo_pattern_add_color_stop_rgba(shade, 0, 0, 0, 0, 0.16);
	cairo_pattern_add_color_stop_rgba(shade, 1, 0, 0, 0, 0);
	cairo_rectangle(cr, 0, 0, w, BAR_H);
	cairo_set_source(cr, shade);
	cairo_fill(cr);
	cairo_pattern_destroy(shade);

	// the left: flame, the app with the keys, the workspaces in use
	flame_mark(ui, 24, cy, 15);
	double x = 46;
	x += shadowed_text(ui, x, cy - 8, app->app_name, F_APP, TEXT, FUI_LEFT, 0) + 18;
	for (int n = 1; n <= WORKSPACES; n++) {
		bool on = n == app->workspace;
		if (!on && !app->used[n]) {
			continue;
		}
		double dw = on ? 16 : 6;
		uint64_t id = fui_id("workspace", (uint64_t)n);
		bool hover = fui_hover(ui, x - 4, 0, dw + 8, BAR_H);
		double lit = fui_ease(ui, id, hover ? 1 : 0, 20);
		uint32_t alpha = on ? 0xf2 : (uint32_t)(0x8c + 0x50 * lit);
		fui_fill_rounded(ui, x, cy - 3, dw, 6, 3, 0xffffff00 | alpha);
		if (hover) {
			fui_cursor_pointer(ui);
		}
		if (fui_clicked(ui, x - 4, 0, dw + 8, BAR_H) && !on) {
			workspace(n);
		}
		x += dw + 7;
	}

	// the right: the time (the notifications open under it), the control
	// center's button, a moon while it's do not disturb
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	char when[64];
	strftime(when, sizeof(when), "%a %-d %b  %H:%M", &tm);
	double tw = fui_text_width(ui, when, F_BAR), tx = w - 18 - tw;
	bool over_clock = fui_hover(ui, tx - 8, cy - 12, tw + 16, 24);
	// the button keeps its place as the minutes go: left of the widest
	// the time is today, not of what it says now
	double room = clock_room(ui, when);
	double clock_lit = fui_ease(ui, fui_id("clock", 0),
		over_clock || app->open == OPEN_LIST ? 1 : 0, 20);
	if (clock_lit > 0.01) {
		fui_fill_rounded(ui, tx - 8, cy - 12, tw + 16, 24, 12,
			0xffffff00 | (uint32_t)(0x33 * clock_lit));
	}
	shadowed_text(ui, w - 18, cy - 8, when, F_BAR, TEXT, FUI_RIGHT, 0);
	double pw = 38, ph = 24, px = w - 18 - room - 14 - pw;
	bool over = fui_hover(ui, px, cy - ph / 2, pw, ph);
	double lit = fui_ease(ui, fui_id("control", 0),
		over || app->open == OPEN_CONTROL ? 1 : 0, 20);
	if (lit > 0.01) {
		fui_fill_rounded(ui, px, cy - ph / 2, pw, ph, ph / 2,
			0xffffff00 | (uint32_t)(0x33 * lit));
	}
	switches_mark(ui, px + pw / 2, cy, TEXT);
	if (notify_quiet(app->notes.notify)) {
		cairo_save(cr);
		cairo_translate(cr, 0, 0.6);
		fui_color(ui, 0x00000059);
		icon_moon(cr, px - 12, cy, 17);
		cairo_restore(cr);
		fui_color(ui, TEXT);
		icon_moon(cr, px - 12, cy, 17);
	}
	if (over || over_clock) {
		fui_cursor_pointer(ui);
	}
	bool toggled = false;
	if (fui_clicked(ui, px, cy - ph / 2, pw, ph)) {
		app->open = app->open == OPEN_CONTROL ? OPEN_NONE : OPEN_CONTROL;
		toggled = true;
	} else if (fui_clicked(ui, tx - 8, cy - 12, tw + 16, 24)) {
		app->open = app->open == OPEN_LIST ? OPEN_NONE : OPEN_LIST;
		toggled = true;
		if (app->open == OPEN_LIST) {
			notify_seen(app->notes.notify); // there they are: no cards after
		}
	}
	if (toggled) {
		// the surface asked to grow for it in this frame, not the next
		fui_redraw(ui);
	}

	// what opens under it: in while open, out after (one goes as the other
	// comes); the surface grown for it till it's gone
	double right = w - 12, top = BAR_H + 6;
	double control = fui_ease(ui, fui_id("control center", 0), app->open == OPEN_CONTROL, 14);
	double list = fui_ease(ui, fui_id("notifications", 0), app->open == OPEN_LIST, 14);
	double list_h = app->open == OPEN_LIST || list > 0.01
		? notes_list_height(ui, &app->notes, LIST_MAX) : 0;
	double needs = fmax(app->open == OPEN_CONTROL || control > 0.01
		? control_height(&app->control) : 0, list_h);
	// as tall as it is now (a list of devices opens in it)
	int tall = (int)(BAR_H + 6 + needs + 16);
	if ((app->open || app->popped) && app->popped < tall) {
		app->popped = tall;
		app->grown = 0;
		fui_panel_popup(ui, tall);
	}
	bool outside = fui_panel_outside(ui);
	struct fui_pane panes[16];
	int n = 0;

	// the cards, under whatever opens: they go while it's there
	struct fui_rect rects[1 + 9] = {{0, 0, w, BAR_H}};
	int n_rects = 1;
	bool covered = app->open || control > 0.01 || list > 0.01;
	// what a media key changed over the cards, the cards moved down for it
	double osd = osd_frame(ui, &app->osd, &app->control, right, top, covered, panes, &n, 16,
		rects, &n_rects, 10);
	double want, cards = notes_cards(ui, &app->notes, right, top + osd, covered, panes, &n, 16,
		rects, &n_rects, 10, &want);
	double osd_in = osd / (OSD_H + 10), on_cards = cards;
	if (osd > 0) {
		want = fmax(want, top + OSD_H);
		cards = fmax(cards, top + OSD_H);
	}

	if (control > 0.01 && fui_height(ui) > BAR_H) {
		// where it stays: it fades in there, nothing moves
		popover(ui, right, top, control, (struct popover_draw){.app = app, .control = true},
			panes, &n);
	}
	if (list > 0.01 && fui_height(ui) > BAR_H) {
		popover(ui, right, top, list, (struct popover_draw){.app = app, .list_h = list_h},
			panes, &n);
	}
	notes_done(&app->notes);
	if (app->open) {
		// a click outside it (and not on its button) closes it
		double ch = app->open == OPEN_CONTROL ? control_height(&app->control) : list_h;
		bool inside = fui_hover(ui, right - CONTROL_W - 12, top, CONTROL_W + 12, ch);
		if (outside || (!inside && !toggled && fui_clicked(ui, 0, 0, w, fui_height(ui)))) {
			app->open = OPEN_NONE;
			fui_redraw(ui); // the next frame starts it going
		}
	}
	fui_set_glass(ui, n ? panes : NULL, n);
	// the glass faded with what opens, all there for the cards
	// the glass faded with what opens or with the popup alone, all there
	// for the cards
	fui_set_glass_opacity(ui, on_cards > 0 ? 1 : fmax(fmax(control, list), osd_in));
	if (!app->open && control <= 0.01 && list <= 0.01 && app->popped) {
		app->popped = 0;
		app->control.picking = -1;
		fui_panel_popup(ui, 0);
	}
	// nothing open: the surface as tall as the cards (grown at once, back
	// when they're all gone), clicks to them and the bar, the rest through
	if (!app->popped) {
		int need = want > 0 ? (int)ceil(want) + 16 : cards > 0 ? app->grown : 0;
		if (need > app->grown || (!need && app->grown)) {
			app->grown = need;
			fui_panel_resize(ui, need);
		}
	}
	fui_set_input(ui, app->grown && !app->popped ? rects : NULL, n_rects);

	// the time's next minute
	fui_after(ui, (int)(60 - tm.tm_sec) * 1000 + 50, clock_tick, app);
}

int main(void) {
	struct app app = {.fd = -1, .control = {.dragging = -1, .picking = -1}};
	snprintf(app.app_name, sizeof(app.app_name), "Flame");
	app.ui = fui_create(&(struct fui_config){
		.app_id = "flamebar",
		.title = "Bar",
		.panel = &(struct fui_panel){
			.edges = FUI_EDGE_TOP | FUI_EDGE_LEFT | FUI_EDGE_RIGHT,
			.room = BAR_H,
		},
		.height = BAR_H,
		.frame = frame,
		.data = &app,
	});
	if (!app.ui) {
		fprintf(stderr, "flamebar: no wayland display\n");
		return 1;
	}
	connect_later(app.ui, &app);
	// what the control center shows, each telling the bar when it changes
	sd_bus *session = bus_open(app.ui, false), *system = bus_open(app.ui, true);
	app.control.sound = sound_start(woken, &app);
	app.control.media = media_start(session, changed, &app);
	app.control.network = network_start(system, changed, &app);
	app.control.bluetooth = bluetooth_start(system, changed, &app);
	app.notes.notify = app.control.notify = notify_start(session, changed, &app);
	app.control.backlight = backlight_start(system);
	int status = fui_run(app.ui);
	// gone before the window: nothing's drawn from them after
	sound_stop(app.control.sound);
	media_stop(app.control.media);
	network_stop(app.control.network);
	bluetooth_stop(app.control.bluetooth);
	notify_stop(app.notes.notify);
	backlight_stop(app.control.backlight);
	app.control = (struct control){0};
	app.notes = (struct notes){0};
	sd_bus_flush_close_unref(session);
	sd_bus_flush_close_unref(system);
	if (app.fd >= 0) {
		close(app.fd);
	}
	fui_destroy(app.ui);
	return status;
}
