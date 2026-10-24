// flamesettings: flame's settings, on flame's glass. what it shows comes
// from `flamectl settings` (flame's ipc), a change goes back as `set KEY
// VALUE` and flame applies it live and keeps it in settings.lua. what
// init.lua sets shows locked: init.lua wins

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../flameui/flameui.h"
#include "../flameipc/ipc.h"
#include "../flameipc/json.h"
#include "browser.h"

// the window's size, what flame made it: read every frame
static int W = 900, H = 600;
#define MIN_W 780
#define MIN_H 480
#define SIDEBAR_W 220
#define CONTENT_X 248
#define CONTENT_W (W - CONTENT_X - 26)

#define TEXT 0xffffffeb
#define TEXT_DIM 0xffffff8c
#define TEXT_FAINT 0xffffff59
#define CARD 0xffffff0d
#define CARD_LINE 0xffffff12

#define F_TITLE "Inter Display Semi-Bold 19"
#define F_LABEL "Inter Medium 10.5"
#define F_TEXT "Inter 10.5"
#define F_SMALL "Inter 9"
#define F_SIDEBAR "Inter Medium 10.5"
#define F_CARD_TITLE "Inter Semi-Bold 9.5"

// ---- what flame says ----

struct app {
	struct fui *ui;
	struct json *settings; // flame's `settings`, NULL when it isn't there
	int page;
	double scroll[8];
	// a change flame refused, shown for a moment
	char toast[160];
	double toast_at;
	// fields being typed into: their text, by key
	struct {
		char key[64];
		char text[128];
	} fields[8];
	int fields_len;
	// searching all the pages at once, and how many rows each had last frame
	char query[64];
	int matches[8];
	// the Wallpaper page's folder and thumbnails
	struct browser browser;
	bool browsing;
	// flamefiles picking a picture: its stdout, what it said so far
	int picking;
	pid_t picker_pid;
	char picked[PATH_MAX];
	size_t picked_len;
};

static void refresh(struct app *app) {
	char *reply = ipc_call("settings");
	struct json *settings = reply ? json_parse(reply) : NULL;
	free(reply);
	if (settings && json_bool(json_get(settings, "ok"), false)) {
		json_free(app->settings);
		app->settings = settings;
	} else {
		json_free(settings);
	}
}

static void redraw_later(struct fui *ui, void *data) {
	fui_redraw(ui);
}

static void refresh_later(struct fui *ui, void *data) {
	struct app *app = data;
	refresh(app);
	fui_redraw(ui);
}

static const struct json *value(struct app *app, const char *key) {
	return json_get(json_get(app->settings, "values"), key);
}

static bool pinned(struct app *app, const char *key) {
	const struct json *list = json_get(app->settings, "pinned");
	for (size_t i = 0; list && i < list->len; i++) {
		if (!strcmp(json_str(json_at(list, i), ""), key)) {
			return true;
		}
	}
	return false;
}

// key to value in flame, kept in our copy right away (flame's answer comes
// with the next refresh), a refusal shown
static void set(struct app *app, const char *key, const char *text) {
	char command[PATH_MAX + 128];
	snprintf(command, sizeof(command), "set %s %s", key, text);
	char *reply = ipc_call(command);
	struct json *answer = reply ? json_parse(reply) : NULL;
	if (!answer || !json_bool(json_get(answer, "ok"), false)) {
		snprintf(app->toast, sizeof(app->toast), "%s: %s", key,
			answer ? json_str(json_get(answer, "error"), "refused") : "flame isn't there");
		app->toast_at = fui_time(app->ui);
	}
	json_free(answer);
	free(reply);
	fui_after(app->ui, 150, refresh_later, app);
}

static void set_number(struct app *app, const char *key, double v, bool whole) {
	char text[32];
	snprintf(text, sizeof(text), whole ? "%.0f" : "%.2f", v);
	// our copy moves now, the slider doesn't snap back while flame reloads
	struct json *values = (struct json *)json_get(app->settings, "values");
	struct json *slot = (struct json *)json_get(values, key);
	if (slot && slot->type == JSON_NUMBER) {
		slot->number = v;
	}
	set(app, key, text);
}

static void set_bool(struct app *app, const char *key, bool v) {
	struct json *slot = (struct json *)value(app, key);
	if (slot && slot->type == JSON_BOOL) {
		slot->boolean = v;
	}
	set(app, key, v ? "true" : "false");
}

static void set_string(struct app *app, const char *key, const char *v) {
	struct json *slot = (struct json *)value(app, key);
	if (slot && slot->type == JSON_STRING) {
		char *copy = strdup(v);
		if (copy) {
			free(slot->string);
			slot->string = copy;
		}
	}
	set(app, key, v);
}

// ---- page layout ----

// where the next row goes, a page drawn top down
struct page {
	struct app *app;
	struct fui *ui;
	double y; // the next row's top
	bool first_row;
	// searching: the page and card being drawn match too, and the rows shown
	const char *query, *section, *card_title;
	int shown;
};

#define ROW_H 52
#define CARD_PAD 16

// needle in hay, any case
static bool has(const char *hay, const char *needle, size_t len) {
	for (; hay && *hay; hay++) {
		if (!strncasecmp(hay, needle, len)) {
			return true;
		}
	}
	return false;
}

// a row shows unless there's a search it doesn't match: every word of it
// somewhere in the row, its card or its page
static bool shown(struct page *p, const char *label, const char *detail, const char *key) {
	const char *q = p->query;
	while (q && *q) {
		while (*q == ' ') {
			q++;
		}
		size_t len = strcspn(q, " ");
		if (len && !has(label, q, len) && !has(detail, q, len) && !has(key, q, len)
				&& !has(p->card_title, q, len) && !has(p->section, q, len)) {
			return false;
		}
		q += len;
	}
	p->shown++;
	return true;
}

// the card behind rows from top to bottom: drawn first, rows go over it
static void card_back(struct page *p, double top, double bottom) {
	fui_fill_rounded(p->ui, CONTENT_X, top, CONTENT_W, bottom - top, 12, CARD);
	fui_rounded(p->ui, CONTENT_X + 0.5, top + 0.5, CONTENT_W - 1, bottom - top - 1, 12);
	fui_color(p->ui, CARD_LINE);
	cairo_set_line_width(fui_cairo(p->ui), 1);
	cairo_stroke(fui_cairo(p->ui));
}

// a row's label (and a line under it, dim), the row's top. controls go at
// the row's right, centered on its middle
static double row(struct page *p, const char *label, const char *detail, bool locked,
		double control_w) {
	double top = p->y;
	if (!p->first_row) {
		fui_fill_rounded(p->ui, CONTENT_X + CARD_PAD, top, CONTENT_W - 2 * CARD_PAD, 1, 0,
			CARD_LINE);
	}
	p->first_row = false;
	double h = detail ? ROW_H + 8 : ROW_H;
	double label_y = detail ? top + 12 : top + (h - 14) / 2 - 1;
	double lw = fui_text(p->ui, CONTENT_X + CARD_PAD, label_y, label, F_LABEL,
		locked ? TEXT_DIM : TEXT, FUI_LEFT, 0);
	if (detail) {
		// up to the control, never under it
		double room = CONTENT_W - 2 * CARD_PAD - (control_w > 0 ? control_w + 24 : 0);
		fui_text(p->ui, CONTENT_X + CARD_PAD, label_y + 19, detail, F_SMALL, TEXT_FAINT,
			FUI_LEFT, room);
	}
	if (locked) {
		double x = CONTENT_X + CARD_PAD + lw + 8;
		double tw = fui_text_width(p->ui, "init.lua", F_SMALL);
		fui_fill_rounded(p->ui, x, label_y + 1, tw + 12, 16, 8, 0xffffff17);
		fui_text(p->ui, x + 6, label_y + 2, "init.lua", F_SMALL, TEXT_DIM, FUI_LEFT, 0);
	}
	p->y += h;
	return top;
}

static double row_mid(double top, bool detail) {
	return top + (detail ? ROW_H + 8 : ROW_H) / 2.0;
}

#define RIGHT (CONTENT_X + CONTENT_W - CARD_PAD)

// ---- rows bound to flame's settings ----

static void toggle_row(struct page *p, const char *key, const char *label, const char *detail) {
	if (!shown(p, label, detail, key)) {
		return;
	}
	bool locked = pinned(p->app, key);
	double top = row(p, label, detail, locked, 40);
	bool on = json_bool(value(p->app, key), false);
	if (fui_toggle(p->ui, fui_id(key, 0), RIGHT - 40, row_mid(top, detail) - 12, &on,
			!locked)) {
		set_bool(p->app, key, on);
	}
}

static void slider_row(struct page *p, const char *key, const char *label, const char *detail,
		double min, double max, double step, const char *fmt, double show_scale,
		const char *zero) {
	if (!shown(p, label, detail, key)) {
		return;
	}
	bool locked = pinned(p->app, key);
	double top = row(p, label, detail, locked, 264);
	double v = json_num(value(p->app, key), min);
	double mid = row_mid(top, detail);
	const double sw = 200;
	if (fui_slider(p->ui, fui_id(key, 0), RIGHT - sw - 64, mid - 11, sw, &v, min, max, step,
			!locked)) {
		set_number(p->app, key, v, step >= 1);
	} else if (v != json_num(value(p->app, key), min)) {
		// dragging: our copy follows, flame hears when it's let go
		struct json *slot = (struct json *)value(p->app, key);
		if (slot) {
			slot->number = v;
		}
	}
	char text[32];
	if (zero && v == 0) {
		snprintf(text, sizeof(text), "%s", zero);
	} else {
		snprintf(text, sizeof(text), fmt, v * show_scale);
	}
	fui_text(p->ui, RIGHT, mid - 8, text, F_TEXT, TEXT_DIM, FUI_RIGHT, 0);
}

static void choice_row(struct page *p, const char *key, const char *label, const char *detail,
		const char *const *labels, const char *const *values, int count) {
	if (!shown(p, label, detail, key)) {
		return;
	}
	bool locked = pinned(p->app, key);
	double top = row(p, label, detail, locked, fui_segmented_width(p->ui, labels, count));
	const char *current = json_str(value(p->app, key), "");
	int selected = -1;
	for (int i = 0; i < count; i++) {
		if (!strcmp(values[i], current)) {
			selected = i;
		}
	}
	if (fui_segmented(p->ui, fui_id(key, 0), RIGHT, row_mid(top, detail) - 14, labels, count,
			&selected, !locked)) {
		set_string(p->app, key, values[selected]);
	}
}

static void color_row(struct page *p, const char *key, const char *label, const char *detail,
		const uint32_t *colors, int count) {
	if (!shown(p, label, detail, key)) {
		return;
	}
	bool locked = pinned(p->app, key);
	double top = row(p, label, detail, locked, count * 32.0);
	const char *current = json_str(value(p->app, key), "");
	int selected = -1;
	for (int i = 0; i < count; i++) {
		char hex[16];
		snprintf(hex, sizeof(hex), "#%06x", colors[i] >> 8);
		if (!strcasecmp(hex, current)) {
			selected = i;
		}
	}
	if (fui_swatches(p->ui, fui_id(key, 0), RIGHT, row_mid(top, detail) - 11, colors, count,
			&selected, !locked)) {
		char hex[16];
		snprintf(hex, sizeof(hex), "#%06x", colors[selected] >> 8);
		set_string(p->app, key, hex);
	}
}

static char *field_text(struct app *app, const char *key) {
	for (int i = 0; i < app->fields_len; i++) {
		if (!strcmp(app->fields[i].key, key)) {
			return app->fields[i].text;
		}
	}
	if (app->fields_len == 8) {
		return app->fields[0].text;
	}
	int i = app->fields_len++;
	snprintf(app->fields[i].key, sizeof(app->fields[i].key), "%s", key);
	snprintf(app->fields[i].text, sizeof(app->fields[i].text), "%s",
		json_str(value(app, key), ""));
	return app->fields[i].text;
}

static void field_row(struct page *p, const char *key, const char *label, const char *detail,
		const char *placeholder) {
	if (!shown(p, label, detail, key)) {
		return;
	}
	bool locked = pinned(p->app, key);
	double top = row(p, label, detail, locked, 220);
	char *text = field_text(p->app, key);
	if (fui_field(p->ui, fui_id(key, 0), RIGHT - 220, row_mid(top, detail) - 14, 220, text, 128,
			placeholder, !locked)) {
		set_string(p->app, key, text);
	}
}

static void info_row(struct page *p, const char *label, const char *text) {
	if (!shown(p, label, text, NULL)) {
		return;
	}
	double top = row(p, label, NULL, false, 0);
	fui_text(p->ui, RIGHT, row_mid(top, false) - 8, text, F_TEXT, TEXT_DIM, FUI_RIGHT,
		CONTENT_W * 0.6);
}

// a card behind rows: the rows draw into a layer of their own first (that's
// where the card ends), the card goes down, the rows over it. each control
// drawn once a frame
typedef void (*rows_fn)(struct page *p);

static void card(struct page *p, const char *title, rows_fn rows) {
	double start = p->y;
	p->y += title ? 22 : 0;
	p->card_title = title;
	p->first_row = true;
	double top = p->y;
	cairo_t *cr = fui_cairo(p->ui);
	cairo_push_group(cr);
	rows(p);
	cairo_pattern_t *drawn = cairo_pop_group(cr);
	p->card_title = NULL;
	if (p->y == top) {
		// a search left none of its rows
		cairo_pattern_destroy(drawn);
		p->y = start;
		return;
	}
	if (title) {
		// searching, every page at once: whose card it is
		char whose[160];
		snprintf(whose, sizeof(whose), "%s%s%s", p->query ? p->section : "",
			p->query ? "  ·  " : "", title);
		for (char *c = whose; *c; c++) {
			*c = (char)toupper((unsigned char)*c);
		}
		fui_text(p->ui, CONTENT_X + 4, start, whose, F_CARD_TITLE, TEXT_DIM, FUI_LEFT, 0);
	}
	card_back(p, top, p->y);
	cairo_set_source(cr, drawn);
	cairo_paint(cr);
	cairo_pattern_destroy(drawn);
	p->y += 18;
}

// ---- pages ----

static const uint32_t focused_colors[] = {0xff6a00ff, 0xff4d8dff, 0xa86bffff, 0x3d8bfdff,
	0x34c759ff, 0xe6e6e6ff};
static const uint32_t border_colors[] = {0x1a1a1aff, 0x2a2a2aff, 0x3a3a3aff, 0x555555ff,
	0x2d2a3eff};
static const uint32_t background_colors[] = {0x000000ff, 0x111111ff, 0x1e1e2eff, 0x0f172aff,
	0x1a1b26ff};

static void appearance_windows(struct page *p) {
	slider_row(p, "gap", "Gaps", "Space between windows and around them", 0, 40, 1, "%.0f px", 1,
		NULL);
	slider_row(p, "border_width", "Border", NULL, 0, 10, 1, "%.0f px", 1, "None");
	color_row(p, "border_focused_color", "Focused window", NULL, focused_colors, 6);
	color_row(p, "border_color", "Other windows", NULL, border_colors, 5);
}

static void appearance_motion(struct page *p) {
	toggle_row(p, "animations.enabled", "Animations",
		"Opening, closing, moving, switching workspaces");
	slider_row(p, "animations.unfocused_opacity", "Unfocused windows", "How see-through a window "
		"is while another has focus", 0.5, 1, 0.05, "%.0f%%", 100, NULL);
}

static void appearance_desktop(struct page *p) {
	color_row(p, "background", "Background", NULL, background_colors, 5);
}

static void page_appearance(struct page *p) {
	card(p, "WINDOWS", appearance_windows);
	card(p, "MOTION", appearance_motion);
	card(p, "DESKTOP", appearance_desktop);
}

static int display_index;

static void display_rows(struct page *p) {
	const struct json *output = json_at(json_get(p->app->settings, "outputs"),
		(size_t)display_index);
	const char *name = json_str(json_get(output, "name"), "?");
	char text[96];
	snprintf(text, sizeof(text), "%.0f × %.0f  ·  %.0f Hz", json_num(json_get(output, "width"), 0),
		json_num(json_get(output, "height"), 0), json_num(json_get(output, "refresh"), 0));
	info_row(p, "Resolution", text);
	char key[96];
	snprintf(key, sizeof(key), "output.%s.scale", name);
	static const char *const labels[] = {"100%", "125%", "150%", "175%", "200%"};
	static const double scales[] = {1, 1.25, 1.5, 1.75, 2};
	if (!shown(p, "Scale", "Bigger text and windows, sharp at any of these", key)) {
		return;
	}
	bool locked = pinned(p->app, key);
	double top = row(p, "Scale", "Bigger text and windows, sharp at any of these", locked,
		fui_segmented_width(p->ui, labels, 5));
	double scale = json_num(json_get(output, "scale"), 1);
	int selected = -1;
	for (int i = 0; i < 5; i++) {
		if (fabs(scales[i] - scale) < 0.01) {
			selected = i;
		}
	}
	if (fui_segmented(p->ui, fui_id(key, 0), RIGHT, row_mid(top, true) - 14, labels, 5, &selected,
			!locked)) {
		set_number(p->app, key, scales[selected], false);
	}
}

static void display_sync(struct page *p) {
	static const char *const labels[] = {"Off", "Fullscreen", "Always"};
	static const char *const values[] = {"off", "fullscreen", "always"};
	choice_row(p, "vrr", "Variable refresh rate", "The screen waits for frames, not frames for "
		"it", labels, values, 3);
	toggle_row(p, "allow_tearing", "Tearing for games", "A fullscreen game with vsync off shows "
		"its frames the moment they're done");
}

static void page_displays(struct page *p) {
	const struct json *outputs = json_get(p->app->settings, "outputs");
	for (size_t i = 0; outputs && i < outputs->len; i++) {
		display_index = (int)i;
		const struct json *output = json_at(outputs, i);
		char title[160];
		snprintf(title, sizeof(title), "%s", json_str(json_get(output, "name"), "SCREEN"));
		card(p, title, display_rows);
	}
	card(p, "SYNC", display_sync);
}

static void games_pacing(struct page *p) {
	static const char *const labels[] = {"Off", "With VRR", "Always"};
	static const char *const values[] = {"off", "vrr", "always"};
	choice_row(p, "pace", "Frame pacing", "A fullscreen game's frames, evenly spaced",
		labels, values, 3);
	slider_row(p, "pace_fps", "Frame rate cap", NULL, 0, 360, 1, "%.0f fps", 1, "No cap");
}

static void games_x(struct page *p) {
	toggle_row(p, "x_zero_scaling", "Sharp X apps", "At a screen's scale X apps draw at its "
		"pixels, they may come out small");
}

static void page_games(struct page *p) {
	card(p, "PACING", games_pacing);
	card(p, "X APPS", games_x);
}

static void input_keyboard(struct page *p) {
	field_row(p, "keyboard.layout", "Layouts", "Comma separated, us,ara", "us");
	field_row(p, "keyboard.options", "Options", "Switching between them, grp:alt_shift_toggle",
		"none");
	slider_row(p, "repeat_rate", "Key repeat", NULL, 5, 80, 1, "%.0f / s", 1, NULL);
	slider_row(p, "repeat_delay", "Delay until repeat", NULL, 150, 1000, 10, "%.0f ms", 1, NULL);
}

static void input_mouse(struct page *p) {
	toggle_row(p, "focus_follows_mouse", "Focus follows the mouse",
		"The window under the pointer gets the keyboard");
	slider_row(p, "cursor_size", "Cursor size", NULL, 16, 64, 2, "%.0f px", 1, NULL);
	field_row(p, "cursor_theme", "Cursor theme", NULL, "default");
}

static void page_input(struct page *p) {
	card(p, "KEYBOARD", input_keyboard);
	card(p, "MOUSE", input_mouse);
}

static void privacy_keys(struct page *p) {
	static const char *const labels[] = {"None", "Modifiers", "Shortcuts", "All"};
	static const char *const values[] = {"never", "modifiers", "combos", "always"};
	choice_row(p, "x_global_keys", "Keys X apps see", "From other apps. All lets any X app "
		"read your typing", labels, values, 4);
}

static void privacy_soon(struct page *p) {
	if (!shown(p, "App permissions", "Apps ask once for your screen or your keys, you decide "
			"here. Coming soon", "permissions")) {
		return;
	}
	double top = row(p, "App permissions", "Apps ask once for your screen or your keys, you "
		"decide here. Coming soon", false, 0);
	(void)top;
}

static void page_privacy(struct page *p) {
	card(p, "X APPS", privacy_keys);
	card(p, "PERMISSIONS", privacy_soon);
}

static void about_rows(struct page *p) {
	info_row(p, "Saved in", json_str(json_get(p->app->settings, "settings_file"), "?"));
	info_row(p, "Wins over these", json_str(json_get(p->app->settings, "init_file"), "?"));
}

static void page_about(struct page *p) {
	card(p, "FLAME", about_rows);
	if (p->query) {
		return; // searching: only the rows
	}
	fui_paragraph(p->ui, CONTENT_X + 4, p->y, CONTENT_W - 8, "Every change here applies right "
		"away and is kept in settings.lua. Anything init.lua sets wins and shows locked.",
		F_SMALL, TEXT_FAINT);
	p->y += 40;
}

// ---- wallpaper ----

#define THUMB_W 320 // thumbnails' pixels, whatever the tiles: a resize doesn't make them again
#define THUMB_H 200

static const char *home_dir(void) {
	const char *home = getenv("HOME");
	return home && *home ? home : "/";
}

// where the browser starts: the wallpaper's folder, else ~/walls, ~/Pictures, ~
static void browse_somewhere(struct app *app) {
	char dir[PATH_MAX];
	const char *current = json_str(value(app, "wallpaper"), "");
	const char *slash = strrchr(current, '/');
	if (slash && slash > current) {
		snprintf(dir, sizeof(dir), "%.*s", (int)(slash - current), current);
		if (browser_open(&app->browser, dir)) {
			return;
		}
	}
	const char *tries[] = {"walls", "Pictures/Wallpapers", "Pictures", ""};
	for (size_t i = 0; i < sizeof(tries) / sizeof(tries[0]); i++) {
		snprintf(dir, sizeof(dir), "%s%s%s", home_dir(), *tries[i] ? "/" : "", tries[i]);
		if (browser_open(&app->browser, dir)) {
			return;
		}
	}
}

// a thumbnail (or what stands for it) in a rounded box
static void picture_in(struct page *p, const char *path, double x, double y, double w, double h,
		double r) {
	cairo_t *cr = fui_cairo(p->ui);
	bool failed = false;
	cairo_surface_t *thumb = path && *path
		? fui_thumb(p->ui, path, THUMB_W, THUMB_H, &failed) : NULL;
	cairo_save(cr);
	fui_rounded(p->ui, x, y, w, h, r);
	cairo_clip(cr);
	if (thumb) {
		cairo_translate(cr, x, y);
		cairo_scale(cr, w / THUMB_W, h / THUMB_H);
		cairo_set_source_surface(cr, thumb, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
		cairo_paint(cr);
	} else {
		fui_color(p->ui, 0xffffff0d);
		cairo_paint(cr);
		if (failed) {
			fui_text(p->ui, x + w / 2, y + h / 2 - 8, "Can't read it", F_SMALL, TEXT_FAINT,
				FUI_CENTER, w - 8);
		}
	}
	cairo_restore(cr);
}

static void wallpaper_current(struct page *p) {
	if (!shown(p, "Wallpaper", "Fill Fit Center Stretch picture desktop", "wallpaper")) {
		return;
	}
	struct fui *ui = p->ui;
	bool locked = pinned(p->app, "wallpaper");
	const char *path = json_str(value(p->app, "wallpaper"), "");
	const double top = p->y, pw = 176, ph = 110, x = CONTENT_X + CARD_PAD;
	if (*path) {
		picture_in(p, path, x, top + 14, pw, ph, 8);
	} else {
		fui_fill_rounded(ui, x, top + 14, pw, ph, 8, 0xffffff0a);
		fui_text(ui, x + pw / 2, top + 14 + ph / 2 - 8, "No wallpaper", F_SMALL, TEXT_FAINT,
			FUI_CENTER, 0);
	}
	const double tx = x + pw + 20, room = RIGHT - tx;
	const char *slash = strrchr(path, '/');
	fui_text(ui, tx, top + 22, *path ? (slash ? slash + 1 : path) : "Just the background color",
		F_LABEL, locked ? TEXT_DIM : TEXT, FUI_LEFT, room);
	if (*path) {
		char folder[PATH_MAX];
		snprintf(folder, sizeof(folder), "%.*s", slash ? (int)(slash - path) : 0, path);
		size_t hl = strlen(home_dir());
		bool under_home = !strncmp(folder, home_dir(), hl) && hl > 1;
		char shown_folder[PATH_MAX];
		snprintf(shown_folder, sizeof(shown_folder), "%s%s", under_home ? "~" : "",
			folder + (under_home ? hl : 0));
		fui_text(ui, tx, top + 42, shown_folder, F_SMALL, TEXT_FAINT, FUI_LEFT, room);
	}
	if (locked) {
		fui_text(ui, tx, top + 62, "init.lua sets it", F_SMALL, TEXT_DIM, FUI_LEFT, room);
	}
	static const char *const labels[] = {"Fill", "Fit", "Center", "Stretch"};
	static const char *const modes[] = {"fill", "fit", "center", "stretch"};
	const char *mode = json_str(value(p->app, "wallpaper_mode"), "fill");
	int selected = 0;
	for (int i = 0; i < 4; i++) {
		if (!strcmp(mode, modes[i])) {
			selected = i;
		}
	}
	bool mode_locked = pinned(p->app, "wallpaper_mode");
	double sw = fui_segmented_width(ui, labels, 4);
	if (fui_segmented(ui, fui_id("wallpaper_mode", 0), tx + sw, top + 92, labels, 4, &selected,
			!mode_locked && *path)) {
		set_string(p->app, "wallpaper_mode", modes[selected]);
	}
	if (*path && !locked && fui_button(ui, fui_id("wallpaper none", 0), tx + sw + 12, top + 92,
			"None")) {
		// back to no wallpaper: flame's default
		char *reply = ipc_call("unset wallpaper");
		free(reply);
		fui_after(ui, 150, refresh_later, p->app);
	}
	p->y = top + ph + 28;
}

static void picked_wallpaper(struct fui *ui, int fd, void *data);

// flamefiles asked to pick a picture, from the folder open. it says the
// path on its stdout, read down a pipe the window's loop watches
static void browse(struct app *app) {
	if (app->picking >= 0) {
		return; // one open already
	}
	int out[2];
	if (pipe(out) < 0) {
		return;
	}
	pid_t pid = fork();
	if (pid == 0) {
		dup2(out[1], STDOUT_FILENO);
		close(out[0]);
		close(out[1]);
		// next to flamesettings first, flame's own build, then $PATH
		char self[PATH_MAX];
		ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
		if (n > 0) {
			self[n] = '\0';
			char *slash = strrchr(self, '/');
			if (slash) {
				snprintf(slash + 1, sizeof(self) - (size_t)(slash + 1 - self), "flamefiles");
				execl(self, "flamefiles", "--pick", "--images", app->browser.dir, (char *)NULL);
			}
		}
		execlp("flamefiles", "flamefiles", "--pick", "--images", app->browser.dir, (char *)NULL);
		_exit(127);
	}
	close(out[1]);
	if (pid < 0) {
		close(out[0]);
		return;
	}
	app->picking = out[0];
	app->picker_pid = pid;
	app->picked_len = 0;
	fui_watch(app->ui, out[0], picked_wallpaper, app);
}

static void picked_wallpaper(struct fui *ui, int fd, void *data) {
	struct app *app = data;
	ssize_t n = read(fd, app->picked + app->picked_len, sizeof(app->picked) - 1 - app->picked_len);
	if (n > 0) {
		app->picked_len += (size_t)n;
		return;
	}
	// it's gone: what it said, if anything, is the wallpaper
	fui_unwatch(ui, fd);
	close(fd);
	app->picking = -1;
	waitpid(app->picker_pid, NULL, 0);
	app->picked[app->picked_len] = '\0';
	app->picked[strcspn(app->picked, "\n")] = '\0';
	if (app->picked[0] == '/') {
		set_string(app, "wallpaper", app->picked);
		char dir[PATH_MAX];
		snprintf(dir, sizeof(dir), "%s", app->picked);
		char *slash = strrchr(dir, '/');
		if (slash && slash != dir) {
			*slash = '\0';
			browser_open(&app->browser, dir);
		}
	}
	fui_redraw(ui);
}

// the folder the pictures are from, and a way to any other
static void wallpaper_path(struct page *p) {
	struct fui *ui = p->ui;
	struct browser *b = &p->app->browser;
	const double y = p->y;
	const char *home = home_dir();
	size_t hl = strlen(home);
	bool under_home = hl > 1 && !strncmp(b->dir, home, hl) && (!b->dir[hl] || b->dir[hl] == '/');
	char shown[PATH_MAX];
	snprintf(shown, sizeof(shown), "%s%s", under_home ? "~" : "", b->dir + (under_home ? hl : 0));
	double bw = fui_text_width(ui, "Browse…", "Inter Medium 10.5") + 28;
	fui_text(ui, CONTENT_X + 4, y + 6, shown, F_LABEL, TEXT_DIM, FUI_LEFT,
		CONTENT_W - bw - 20);
	if (fui_button(ui, fui_id("browse", 0), RIGHT + CARD_PAD - bw, y, "Browse…")) {
		browse(p->app);
	}
	p->y += 40;
}

// does every word of the search show up in name
static bool name_matches(const char *name, const char *query) {
	const char *q = query;
	while (q && *q) {
		while (*q == ' ') {
			q++;
		}
		size_t len = strcspn(q, " ");
		if (len && !has(name, q, len)) {
			return false;
		}
		q += len;
	}
	return true;
}

// the folder's pictures, tiles in rows. searching: only the ones whose
// names match
static void wallpaper_grid(struct page *p) {
	struct fui *ui = p->ui;
	struct app *app = p->app;
	struct browser *b = &app->browser;
	const char *current = json_str(value(app, "wallpaper"), "");
	bool locked = pinned(app, "wallpaper");
	const double gap = 14, label_h = 24;
	int cols = (int)((CONTENT_W + gap) / (170 + gap));
	cols = cols < 2 ? 2 : cols;
	const double tw = (CONTENT_W - (cols - 1) * gap) / cols, th = tw * THUMB_H / THUMB_W;
	int col = 0, count = 0;
	double y = p->y;
	for (size_t i = 0; i < b->len; i++) {
		const struct browser_entry *e = &b->entries[i];
		if (e->folder || (p->query && !name_matches(e->name, p->query))) {
			continue; // pictures only: other folders are Browse's
		}
		count++;
		char path[PATH_MAX];
		snprintf(path, sizeof(path), "%s%s%s", b->dir, strcmp(b->dir, "/") ? "/" : "", e->name);
		double x = CONTENT_X + col * (tw + gap);
		// only what's on screen gets drawn, and thumbnails asked for
		if (y + th + label_h > 0 && y < H) {
			bool hover = fui_hover(ui, x, y, tw, th) && !locked;
			bool picked = !strcmp(path, current);
			double lift = fui_ease(ui, fui_id(path, 7), hover ? 1 : 0, 18);
			picture_in(p, path, x, y, tw, th, 10);
			if (picked || lift > 0.01) {
				fui_rounded(ui, x - 3, y - 3, tw + 6, th + 6, 13);
				fui_color(ui, picked ? 0xff7a2aff : (uint32_t)(0xffffff00 | (uint32_t)(0x50 * lift)));
				cairo_set_line_width(fui_cairo(ui), 2.5);
				cairo_stroke(fui_cairo(ui));
			}
			fui_text(ui, x + tw / 2, y + th + 5, e->name, F_SMALL, picked ? TEXT : TEXT_DIM,
				FUI_CENTER, tw);
			if (hover) {
				fui_cursor_pointer(ui);
			}
			if (hover && fui_clicked(ui, x, y, tw, th)) {
				set_string(app, "wallpaper", path);
			}
		}
		if (++col == cols) {
			col = 0;
			y += th + label_h + gap;
		}
	}
	if (col) {
		y += th + label_h + gap;
	}
	if (!count && !p->query) {
		fui_text(ui, CONTENT_X + 4, y, "No pictures here", F_TEXT, TEXT_DIM,
			FUI_LEFT, 0);
		y += 30;
	}
	p->shown += p->query ? count : 0;
	p->y = y;
}

static void page_wallpaper(struct page *p) {
	if (!p->app->browsing) {
		browse_somewhere(p->app);
		p->app->browsing = true;
	}
	card(p, p->query ? "ON THE DESKTOP" : NULL, wallpaper_current);
	if (p->query) {
		// the open folder's pictures that match, under whose they are
		double start = p->y;
		int before = p->shown;
		p->y += 22;
		wallpaper_grid(p);
		if (p->shown == before) {
			p->y = start;
		} else {
			const char *slash = strrchr(p->app->browser.dir, '/');
			char title[160];
			snprintf(title, sizeof(title), "WALLPAPER  ·  %s", slash && slash[1] ? slash + 1
				: p->app->browser.dir);
			for (char *c = title; *c; c++) {
				*c = (char)toupper((unsigned char)*c);
			}
			fui_text(p->ui, CONTENT_X + 4, start, title, F_CARD_TITLE, TEXT_DIM, FUI_LEFT, 0);
		}
		return;
	}
	wallpaper_path(p);
	wallpaper_grid(p);
}

// ---- the window ----

enum icon {
	ICON_DISPLAY,
	ICON_APPEARANCE,
	ICON_WALLPAPER,
	ICON_GAMES,
	ICON_KEYBOARD,
	ICON_PRIVACY,
	ICON_ABOUT,
};

static const struct {
	const char *name, *title;
	enum icon icon;
	uint32_t color;
	void (*draw)(struct page *p);
} pages[] = {
	{"Appearance", "Appearance", ICON_APPEARANCE, 0xa855f7ff, page_appearance},
	{"Wallpaper", "Wallpaper", ICON_WALLPAPER, 0x14b8a6ff, page_wallpaper},
	{"Displays", "Displays", ICON_DISPLAY, 0x3b82f6ff, page_displays},
	{"Games", "Games", ICON_GAMES, 0xef4444ff, page_games},
	{"Keyboard & Mouse", "Keyboard & Mouse", ICON_KEYBOARD, 0x737a86ff, page_input},
	{"Privacy & Security", "Privacy & Security", ICON_PRIVACY, 0x0ea5e9ff, page_privacy},
	{"About", "About flame", ICON_ABOUT, 0x8b9099ff, page_about},
};
#define PAGES ((int)(sizeof(pages) / sizeof(pages[0])))

// a white glyph on a colored square, like the ones on a phone
static void icon(struct fui *ui, enum icon kind, double x, double y, uint32_t color) {
	cairo_t *cr = fui_cairo(ui);
	fui_fill_rounded(ui, x, y, 22, 22, 6, color);
	fui_color(ui, 0xffffffff);
	cairo_set_line_width(cr, 1.6);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	double cx = x + 11, cy = y + 11;
	switch (kind) {
	case ICON_DISPLAY:
		fui_rounded(ui, cx - 6.5, cy - 5, 13, 8.5, 1.5);
		cairo_stroke(cr);
		cairo_move_to(cr, cx - 3, cy + 6.2);
		cairo_line_to(cr, cx + 3, cy + 6.2);
		cairo_stroke(cr);
		break;
	case ICON_APPEARANCE:
		cairo_arc(cr, cx, cy, 6, 0, 2 * M_PI);
		cairo_stroke(cr);
		cairo_arc(cr, cx, cy, 6, M_PI / 2, 3 * M_PI / 2);
		cairo_close_path(cr);
		cairo_fill(cr);
		break;
	case ICON_GAMES:
		fui_rounded(ui, cx - 7, cy - 4, 14, 8.5, 4);
		cairo_stroke(cr);
		cairo_move_to(cr, cx - 4.2, cy);
		cairo_line_to(cr, cx - 1.6, cy);
		cairo_move_to(cr, cx - 2.9, cy - 1.3);
		cairo_line_to(cr, cx - 2.9, cy + 1.3);
		cairo_stroke(cr);
		cairo_arc(cr, cx + 3.2, cy, 1.1, 0, 2 * M_PI);
		cairo_fill(cr);
		break;
	case ICON_KEYBOARD:
		fui_rounded(ui, cx - 7, cy - 4.5, 14, 9, 2);
		cairo_stroke(cr);
		for (int i = 0; i < 4; i++) {
			cairo_arc(cr, cx - 4.2 + i * 2.8, cy - 1.2, 0.7, 0, 2 * M_PI);
			cairo_fill(cr);
		}
		cairo_move_to(cr, cx - 3, cy + 2);
		cairo_line_to(cr, cx + 3, cy + 2);
		cairo_stroke(cr);
		break;
	case ICON_PRIVACY:
		fui_rounded(ui, cx - 5, cy - 1, 10, 7.5, 1.5);
		cairo_fill(cr);
		cairo_arc(cr, cx, cy - 2, 3.4, M_PI, 2 * M_PI);
		cairo_stroke(cr);
		break;
	case ICON_WALLPAPER:
		// a picture: hills under a sun
		fui_rounded(ui, cx - 7, cy - 5.5, 14, 11, 2);
		cairo_stroke(cr);
		cairo_move_to(cr, cx - 6.2, cy + 4);
		cairo_line_to(cr, cx - 2.2, cy - 0.5);
		cairo_line_to(cr, cx + 0.8, cy + 2.2);
		cairo_line_to(cr, cx + 2.8, cy + 0.6);
		cairo_line_to(cr, cx + 6.2, cy + 4);
		cairo_close_path(cr);
		cairo_fill(cr);
		cairo_arc(cr, cx + 3, cy - 2.3, 1.3, 0, 2 * M_PI);
		cairo_fill(cr);
		break;
	case ICON_ABOUT:
		cairo_arc(cr, cx, cy - 4, 1.2, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_move_to(cr, cx, cy - 0.8);
		cairo_line_to(cr, cx, cy + 5);
		cairo_stroke(cr);
		break;
	}
}

// where scroll and fades are kept for the search, past the pages
#define SEARCH_SLOT 7

static void sidebar(struct app *app) {
	struct fui *ui = app->ui;
	const double x = 10, w = SIDEBAR_W - 20, item_h = 30, top = 54;
	fui_search(ui, fui_id("search", 0), x, 14, w, app->query, sizeof(app->query), true);
	bool searching = app->query[0] != '\0';
	// the highlight glides to the page that's open, gone while searching
	double sel_y = fui_ease(ui, fui_id("selection", 0), top + app->page * (item_h + 2), 22);
	double sel = fui_ease(ui, fui_id("selection", 1), searching ? 0 : 1, 18);
	if (sel > 0.01) {
		fui_fill_rounded(ui, x, sel_y, w, item_h, 8, (uint32_t)(0xff7a2a00 | (uint32_t)(0xff * sel)));
	}
	for (int i = 0; i < PAGES; i++) {
		double y = top + i * (item_h + 2);
		bool hover = fui_hover(ui, x, y, w, item_h);
		bool current = i == app->page && !searching;
		double lit = fui_ease(ui, fui_id(pages[i].name, 1), hover && !current ? 1 : 0, 20);
		if (lit > 0.01) {
			fui_fill_rounded(ui, x, y, w, item_h, 8, (uint32_t)(0xffffff00 | (uint32_t)(0x14 * lit)));
		}
		if (hover) {
			fui_cursor_pointer(ui);
		}
		if (fui_clicked(ui, x, y, w, item_h)) {
			// a page picked: the search is done with
			app->page = i;
			app->query[0] = '\0';
		}
		// searching: the pages nothing was found on fade back
		double found = fui_ease(ui, fui_id(pages[i].name, 2),
			!searching || app->matches[i] ? 1 : 0.35, 16);
		cairo_t *cr = fui_cairo(ui);
		cairo_push_group(cr);
		icon(ui, pages[i].icon, x + 6, y + 4, pages[i].color);
		fui_text(ui, x + 38, y + 6, pages[i].name, F_SIDEBAR, TEXT, FUI_LEFT, w - 46);
		cairo_pop_group_to_source(cr);
		cairo_paint_with_alpha(cr, found);
	}
}

static void content(struct app *app) {
	struct fui *ui = app->ui;
	cairo_t *cr = fui_cairo(ui);
	if (!app->settings) {
		fui_text(ui, CONTENT_X + CONTENT_W / 2.0, H / 2.0 - 20, "flame isn't running",
			F_TITLE, TEXT, FUI_CENTER, 0);
		fui_text(ui, CONTENT_X + CONTENT_W / 2.0, H / 2.0 + 10, "these are flame's settings, "
			"start flame and open them again", F_TEXT, TEXT_DIM, FUI_CENTER, 0);
		return;
	}
	bool searching = app->query[0] != '\0';
	int slot = searching ? SEARCH_SLOT : app->page;
	// a page switching fades in from a little lower
	double shown = fui_ease(ui, fui_id("page", (uint64_t)slot), 1, 14);
	double eased = fui_scroll_begin(ui, fui_id("scroll", (uint64_t)slot), CONTENT_X - 12, 0,
		W - CONTENT_X + 12, H, &app->scroll[slot]);
	cairo_push_group(cr);
	struct page p = {.app = app, .ui = ui, .y = 24 - eased + (1 - shown) * 10};
	if (searching) {
		// every page at once, only what matches
		fui_text(ui, CONTENT_X + 4, p.y, "Search", F_TITLE, TEXT, FUI_LEFT, 0);
		p.y += 48;
		p.query = app->query;
		int total = 0;
		for (int i = 0; i < PAGES; i++) {
			p.section = pages[i].name;
			p.shown = 0;
			pages[i].draw(&p);
			app->matches[i] = p.shown;
			total += p.shown;
		}
		if (!total) {
			char none[160];
			snprintf(none, sizeof(none), "Nothing matches “%s”", app->query);
			fui_text(ui, CONTENT_X + 4, p.y, none, F_TEXT, TEXT_DIM, FUI_LEFT, CONTENT_W - 8);
			p.y += 30;
		}
	} else {
		fui_text(ui, CONTENT_X + 4, p.y, pages[app->page].title, F_TITLE, TEXT, FUI_LEFT, 0);
		p.y += 48;
		p.section = pages[app->page].name;
		pages[app->page].draw(&p);
	}
	cairo_pop_group_to_source(cr);
	cairo_paint_with_alpha(cr, shown);
	// down to the last row, room under it
	fui_scroll_end(ui, p.y + eased - (1 - shown) * 10 + 20);

	// a refusal, for a few seconds
	double age = fui_time(ui) - app->toast_at;
	if (app->toast[0] && age < 3.5) {
		double fade = age < 3 ? 1 : (3.5 - age) / 0.5;
		double tw = fui_text_width(ui, app->toast, F_TEXT) + 28;
		double tx = CONTENT_X + (CONTENT_W - tw) / 2, ty = H - 52;
		fui_fill_rounded(ui, tx, ty, tw, 30, 15, (uint32_t)(0x3a1d1d00 | (uint32_t)(0xe6 * fade)));
		fui_text(ui, tx + 14, ty + 7, app->toast, F_TEXT, (uint32_t)(0xffffff00 |
			(uint32_t)(0xeb * fade)), FUI_LEFT, 0);
		fui_after(ui, 50, redraw_later, app); // keeps it fading
	}
}

static void frame(struct fui *ui, void *data) {
	struct app *app = data;
	W = fui_width(ui);
	H = fui_height(ui);
	// the window: one pane of frosted glass, the desktop glowing through it
	// vivid, and over its right part the content's solid dark: the sidebar
	// is what's left of the glass
	struct fui_pane pane = {0, 0, W, H, 18, 40, 0x1e1e2470, 1.9, 0.25, 0, 0};
	fui_set_glass(ui, &pane, 1);
	cairo_t *cr = fui_cairo(ui);
	if (!fui_has_glass(ui)) {
		fui_fill_rounded(ui, 0, 0, W, H, 18, 0x2a2a30ff); // no flame glass: plain
	}
	const double r = 18;
	cairo_new_path(cr);
	cairo_move_to(cr, SIDEBAR_W, 0);
	cairo_arc(cr, W - r, r, r, -M_PI / 2, 0);
	cairo_arc(cr, W - r, H - r, r, 0, M_PI / 2);
	cairo_line_to(cr, SIDEBAR_W, H);
	cairo_close_path(cr);
	fui_color(ui, 0x1c1c20ff);
	cairo_fill(cr);
	fui_fill_rounded(ui, SIDEBAR_W, 0, 1, H, 0, 0x00000066);
	fui_rounded(ui, 0.5, 0.5, W - 1, H - 1, r);
	fui_color(ui, 0xffffff1c);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
	sidebar(app);
	content(app);
}

int main(int argc, char **argv) {
	struct app app = {.picking = -1};
	for (int i = 1; i < argc; i++) {
		for (int p = 0; p < PAGES; p++) {
			if (!strcasecmp(argv[i], pages[p].name) || (!strncasecmp(argv[i], pages[p].name, 4))) {
				app.page = p;
			}
		}
	}
	refresh(&app);
	app.ui = fui_create(&(struct fui_config){
		.app_id = "flamesettings",
		.title = "Settings",
		.width = W,
		.height = H,
		.min_width = MIN_W,
		.min_height = MIN_H,
		.corner_radius = 18,
		.frame = frame,
		.data = &app,
	});
	if (!app.ui) {
		fprintf(stderr, "flamesettings: no wayland display\n");
		return 1;
	}
	browser_init(&app.browser, app.ui);
	int status = fui_run(app.ui);
	browser_finish(&app.browser);
	fui_destroy(app.ui);
	json_free(app.settings);
	return status;
}
