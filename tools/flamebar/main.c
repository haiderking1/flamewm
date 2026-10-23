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

#define BAR_H 32
#define TEXT 0xffffffff
#define TEXT_DIM 0xffffffb3
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
	bool control_open;
	int popped; // how tall the surface was grown for it (open, or closing), 0: not
	struct control control;
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

static void took_state(struct app *app, const char *text) {
	struct json *state = json_parse(text);
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
	if (app->fd < 0 || write(app->fd, "subscribe\n", 10) != 10 || !fui_watch(ui, app->fd,
			readable, app)) {
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

static void clock_tick(struct fui *ui, void *data) {
	fui_redraw(ui);
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

	// the right: the time, the control center's button
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	char when[64];
	strftime(when, sizeof(when), "%a %-d %b  %H:%M", &tm);
	double rx = w - 18;
	rx -= shadowed_text(ui, rx, cy - 8, when, F_BAR, TEXT, FUI_RIGHT, 0) + 14;
	double pw = 38, ph = 24, px = rx - pw;
	bool over = fui_hover(ui, px, cy - ph / 2, pw, ph);
	double lit = fui_ease(ui, fui_id("control", 0), over || app->control_open ? 1 : 0, 20);
	if (lit > 0.01) {
		fui_fill_rounded(ui, px, cy - ph / 2, pw, ph, ph / 2,
			0xffffff00 | (uint32_t)(0x33 * lit));
	}
	switches_mark(ui, px + pw / 2, cy, TEXT);
	if (over) {
		fui_cursor_pointer(ui);
	}
	bool toggled = fui_clicked(ui, px, cy - ph / 2, pw, ph);
	if (toggled) {
		// the surface asked to grow for it in this frame, not the next
		app->control_open = !app->control_open;
		fui_redraw(ui);
	}

	// the control center: in while open, out after; the surface grown for
	// it till it's gone
	double shown = fui_ease(ui, fui_id("control center", 0), app->control_open ? 1 : 0, 14);
	// as tall as it is now (a list of devices opens in it)
	int tall = (int)(BAR_H + 6 + control_height(&app->control) + 16);
	if ((app->control_open || app->popped) && app->popped < tall) {
		app->popped = tall;
		fui_panel_popup(ui, tall);
	}
	bool outside = fui_panel_outside(ui);
	struct fui_pane panes[8];
	int n = 0;
	if (shown > 0.01 && fui_height(ui) > BAR_H) {
		// where it stays: it fades in there, nothing moves
		double right = w - 12, top = BAR_H + 6;
		// faded as one: a group the size of it, not of the screen. all
		// there, it's drawn straight in
		bool fading = shown < 1;
		cairo_save(cr);
		cairo_rectangle(cr, right - CONTROL_W - 2, top - 2, CONTROL_W + 4,
			fui_height(ui) - top + 2);
		cairo_clip(cr);
		if (fading) {
			cairo_push_group(cr);
		}
		app->control.room = fui_height(ui) - top - 16;
		n = control_frame(ui, &app->control, right, top, panes, 8);
		if (fading) {
			cairo_pop_group_to_source(cr);
			cairo_paint_with_alpha(cr, shown);
		}
		cairo_restore(cr);
		// a click outside it (and not on its button) closes it
		double ch = control_height(&app->control);
		bool inside = fui_hover(ui, right - CONTROL_W, top, CONTROL_W, ch);
		if (app->control_open && (outside || (!inside && !toggled
				&& fui_clicked(ui, 0, 0, w, fui_height(ui))))) {
			app->control_open = false;
			fui_redraw(ui); // the next frame starts it going
		}
	}
	fui_set_glass(ui, n ? panes : NULL, n);
	fui_set_glass_opacity(ui, shown);
	if (!app->control_open && shown <= 0.01 && app->popped) {
		app->popped = 0;
		app->control.picking = -1;
		fui_panel_popup(ui, 0);
	}

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
	int status = fui_run(app.ui);
	// gone before the window: nothing's drawn from them after
	sound_stop(app.control.sound);
	media_stop(app.control.media);
	network_stop(app.control.network);
	bluetooth_stop(app.control.bluetooth);
	app.control = (struct control){0};
	sd_bus_flush_close_unref(session);
	sd_bus_flush_close_unref(system);
	if (app.fd >= 0) {
		close(app.fd);
	}
	fui_destroy(app.ui);
	return status;
}
