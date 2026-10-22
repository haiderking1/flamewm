// flamesettings: flame's settings, on flame's glass. what it shows comes
// from `flamectl settings` (flame's ipc), a change goes back as `set KEY
// VALUE` and flame applies it live and keeps it in settings.lua. what
// init.lua sets shows locked: init.lua wins

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "../flameui/flameui.h"
#include "json.h"

#define W 900
#define H 600
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

// ---- flame's ipc ----

static int ipc_connect(void) {
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	const char *path = getenv("FLAMEWM_SOCKET");
	if (path) {
		snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	} else {
		const char *runtime = getenv("XDG_RUNTIME_DIR"), *display = getenv("WAYLAND_DISPLAY");
		if (!runtime || !display) {
			return -1;
		}
		snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/flamewm.%s.sock", runtime, display);
	}
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		if (fd >= 0) {
			close(fd);
		}
		return -1;
	}
	return fd;
}

// one command, its one line answer (free it), NULL when flame isn't there
static char *ipc(const char *command) {
	int fd = ipc_connect();
	if (fd < 0) {
		return NULL;
	}
	size_t len = strlen(command);
	if (write(fd, command, len) != (ssize_t)len || write(fd, "\n", 1) != 1) {
		close(fd);
		return NULL;
	}
	size_t cap = 4096, have = 0;
	char *buf = malloc(cap);
	while (buf) {
		if (have + 1 >= cap) {
			cap *= 2;
			char *grown = realloc(buf, cap);
			if (!grown) {
				free(buf);
				buf = NULL;
				break;
			}
			buf = grown;
		}
		ssize_t got = read(fd, buf + have, cap - have - 1);
		if (got < 0 && errno == EINTR) {
			continue;
		}
		if (got <= 0) {
			break;
		}
		have += (size_t)got;
		buf[have] = '\0';
		if (memchr(buf, '\n', have)) {
			break;
		}
	}
	close(fd);
	if (buf) {
		buf[have] = '\0';
		char *nl = strchr(buf, '\n');
		if (nl) {
			*nl = '\0';
		}
	}
	return buf;
}

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
};

static void refresh(struct app *app) {
	char *reply = ipc("settings");
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
	char command[256];
	snprintf(command, sizeof(command), "set %s %s", key, text);
	char *reply = ipc(command);
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
	double card_top; // the card being drawn, 0 for none
	bool first_row;
};

#define ROW_H 52
#define CARD_PAD 16

static void card_begin(struct page *p, const char *title) {
	if (title) {
		fui_text(p->ui, CONTENT_X + 4, p->y, title, F_CARD_TITLE, TEXT_DIM, FUI_LEFT, 0);
		p->y += 22;
	}
	p->card_top = p->y;
	p->first_row = true;
}

static void card_end(struct page *p, double rows_bottom) {
	(void)rows_bottom;
	p->y += 18;
	p->card_top = 0;
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
	bool locked = pinned(p->app, key);
	double top = row(p, label, detail, locked, 220);
	char *text = field_text(p->app, key);
	if (fui_field(p->ui, fui_id(key, 0), RIGHT - 220, row_mid(top, detail) - 14, 220, text, 128,
			placeholder, !locked)) {
		set_string(p->app, key, text);
	}
}

static void info_row(struct page *p, const char *label, const char *text) {
	double top = row(p, label, NULL, false, 0);
	fui_text(p->ui, RIGHT, row_mid(top, false) - 8, text, F_TEXT, TEXT_DIM, FUI_RIGHT,
		CONTENT_W * 0.6);
}

// a card behind rows: the rows draw into a layer of their own first (that's
// where the card ends), the card goes down, the rows over it. each control
// drawn once a frame
typedef void (*rows_fn)(struct page *p);

static void card(struct page *p, const char *title, rows_fn rows) {
	card_begin(p, title);
	double top = p->y;
	cairo_t *cr = fui_cairo(p->ui);
	cairo_push_group(cr);
	rows(p);
	cairo_pattern_t *drawn = cairo_pop_group(cr);
	card_back(p, top, p->y);
	cairo_set_source(cr, drawn);
	cairo_paint(cr);
	cairo_pattern_destroy(drawn);
	card_end(p, p->y);
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
	fui_paragraph(p->ui, CONTENT_X + 4, p->y, CONTENT_W - 8, "Every change here applies right "
		"away and is kept in settings.lua. Anything init.lua sets wins and shows locked.",
		F_SMALL, TEXT_FAINT);
	p->y += 40;
}

// ---- the window ----

enum icon { ICON_DISPLAY, ICON_APPEARANCE, ICON_GAMES, ICON_KEYBOARD, ICON_PRIVACY, ICON_ABOUT };

static const struct {
	const char *name, *title;
	enum icon icon;
	uint32_t color;
	void (*draw)(struct page *p);
} pages[] = {
	{"Appearance", "Appearance", ICON_APPEARANCE, 0xa855f7ff, page_appearance},
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
	case ICON_ABOUT:
		cairo_arc(cr, cx, cy - 4, 1.2, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_move_to(cr, cx, cy - 0.8);
		cairo_line_to(cr, cx, cy + 5);
		cairo_stroke(cr);
		break;
	}
}

static void sidebar(struct app *app) {
	struct fui *ui = app->ui;
	const double top = 16, item_h = 30, x = 10, w = SIDEBAR_W - 20;
	// the highlight glides to the page that's open
	double sel_y = fui_ease(ui, fui_id("selection", 0), top + app->page * (item_h + 2), 22);
	fui_fill_rounded(ui, x, sel_y, w, item_h, 8, 0xff7a2aff);
	for (int i = 0; i < PAGES; i++) {
		double y = top + i * (item_h + 2);
		bool hover = fui_hover(ui, x, y, w, item_h);
		double lit = fui_ease(ui, fui_id(pages[i].name, 1), hover && i != app->page ? 1 : 0, 20);
		if (lit > 0.01) {
			fui_fill_rounded(ui, x, y, w, item_h, 8, (uint32_t)(0xffffff00 | (uint32_t)(0x14 * lit)));
		}
		if (hover) {
			fui_cursor_pointer(ui);
		}
		if (fui_clicked(ui, x, y, w, item_h)) {
			app->page = i;
		}
		icon(ui, pages[i].icon, x + 6, y + 4, pages[i].color);
		fui_text(ui, x + 38, y + 6, pages[i].name, F_SIDEBAR, TEXT, FUI_LEFT, w - 46);
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
	// a page switching fades in from a little lower
	double shown = fui_ease(ui, fui_id("page", (uint64_t)app->page), 1, 14);
	double *scroll = &app->scroll[app->page];
	if (fui_hover(ui, CONTENT_X - 20, 0, W - CONTENT_X + 20, H)) {
		*scroll += fui_scrolled(ui);
	}
	double eased = fui_ease(ui, fui_id("scroll", (uint64_t)app->page), *scroll, 24);

	cairo_save(cr);
	cairo_rectangle(cr, CONTENT_X - 12, 0, W - CONTENT_X + 12, H);
	cairo_clip(cr);
	cairo_push_group(cr);
	struct page p = {.app = app, .ui = ui, .y = 24 - eased + (1 - shown) * 10};
	fui_text(ui, CONTENT_X + 4, p.y, pages[app->page].title, F_TITLE, TEXT, FUI_LEFT, 0);
	p.y += 48;
	pages[app->page].draw(&p);
	cairo_pop_group_to_source(cr);
	cairo_paint_with_alpha(cr, shown);
	cairo_restore(cr);
	// scrolled no further than there is
	double bottom = p.y + eased - (1 - shown) * 10;
	double most = bottom > H - 20 ? bottom - (H - 20) : 0;
	*scroll = *scroll < 0 ? 0 : *scroll > most ? most : *scroll;

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
	// the window: one pane of frosted glass, the desktop glowing through it
	// vivid, and over its right part the content's solid dark: the sidebar
	// is what's left of the glass
	struct fui_pane pane = {0, 0, W, H, 18, 40, 0x1e1e2470, 1.9, 0.25, 0};
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
	struct app app = {0};
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
		.corner_radius = 18,
		.frame = frame,
		.data = &app,
	});
	if (!app.ui) {
		fprintf(stderr, "flamesettings: no wayland display\n");
		return 1;
	}
	int status = fui_run(app.ui);
	fui_destroy(app.ui);
	json_free(app.settings);
	return status;
}
