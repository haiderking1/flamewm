// flamegreet: the login screen, greetd's greeter (flamewm -g flamegreet).
// the time big over the wallpaper, the people who can log in side by side
// under it, a field for the picked one's password, what to log into, and
// sleep, restart and shut down at the bottom. looks like flamelock: what
// greets you is what locks for you

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "../flameui/flameui.h"
#include "../flameipc/ipc.h"
#include "../flamelock/look.h"
#include "greetd.h"
#include "people.h"
#include "power.h"

#define F_DATE "Inter Semi-Bold 20"
#define F_TIME "Inter Semi-Bold 104"
#define F_NAME "Inter Semi-Bold 14"
#define F_OTHER "Inter Medium 12"
#define F_HINT "Inter Medium 10"
#define F_SMALL "Inter Medium 10"
#define AVATAR 80 // the picked one
#define AVATAR_OTHER 60
#define SPACING 120 // between people's middles
#define POWER_R 20
#define AWAKE 10.0
#define PANES_MAX (4 + PEOPLE_MAX + POWER_COUNT)

struct greet {
	struct fui *ui;
	struct people people;
	int person, session;
	int typed_len;
	double active_at, typed_at, shaken_at;
	double mx, my;
	bool started, leaving;
	bool can[POWER_COUNT];
	char hint[256]; // why the last one didn't go, till something's typed
	struct fui_pane panes[PANES_MAX];
	int panes_len;
};

static char typed[LOOK_TYPED_MAX]; // locked in memory, wiped as soon as it's not needed

static void wipe(struct greet *g) {
	explicit_bzero(typed, sizeof(typed));
	g->typed_len = 0;
}

static void minute(struct fui *ui, void *data) {
	fui_redraw(ui);
}

static void pane(struct greet *g, struct fui_pane p) {
	if (g->panes_len < PANES_MAX) {
		g->panes[g->panes_len++] = p;
	}
}

// glass under something round or a pill, the field's kind
static void glass(struct greet *g, double x, double y, double w, double h, uint32_t tint) {
	pane(g, (struct fui_pane){(int)x, (int)y, (int)w, (int)h, h / 2.0, 12, tint, 1.8, 0.22, 18,
		0});
}

static void submit(struct greet *g) {
	// nothing typed: no try at all (a stray enter counting toward pam's
	// lockout after so many wrong ones)
	if (!g->people.len || !g->typed_len || greet_state() != GREET_IDLE || g->leaving) {
		return;
	}
	const struct session *s = &g->people.session[g->session];
	char desktop[160], current[160];
	snprintf(desktop, sizeof(desktop), "XDG_SESSION_DESKTOP=%s", s->id);
	snprintf(current, sizeof(current), "XDG_CURRENT_DESKTOP=%s", *s->desktops ? s->desktops
		: s->id);
	const char *const cmd[] = {"/bin/sh", "-c", s->exec, NULL};
	const char *const env[] = {"XDG_SESSION_TYPE=wayland", desktop, current, NULL};
	greet_login(g->ui, g->people.person[g->person].user, typed,
		&(struct greet_session){cmd, env});
	wipe(g); // the thread has its own copy, wiped after too
	g->hint[0] = '\0';
}

// the picked one's wallpaper behind it, flame fading it in. none of their
// own: the login screen's
static void their_wallpaper(struct greet *g) {
	char command[4200];
	snprintf(command, sizeof(command), "greeter-wallpaper %s",
		g->people.look[g->person].wallpaper);
	free(ipc_call(command));
}

static void pick(struct greet *g, int person) {
	if (person == g->person || greet_state() == GREET_CHECKING) {
		return;
	}
	g->person = person;
	wipe(g);
	g->hint[0] = '\0';
	their_wallpaper(g);
}

// ---- keys ----

static void keys(struct fui *ui, struct greet *g) {
	uint32_t sym;
	unsigned mods;
	const char *text;
	int n = g->people.len;
	for (int i = 0; fui_key_at(ui, i, &sym, &mods, &text); i++) {
		g->active_at = g->typed_at = fui_time(ui);
		if (greet_state() != GREET_IDLE || g->leaving || !n) {
			continue;
		}
		// someone else: tab, or the arrows while nothing's typed
		bool shift = mods & FUI_SHIFT;
		if (sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab) {
			pick(g, (g->person + (shift || sym == XKB_KEY_ISO_Left_Tab ? n - 1 : 1)) % n);
			continue;
		}
		if (!g->typed_len && (sym == XKB_KEY_Left || sym == XKB_KEY_Right)) {
			pick(g, (g->person + (sym == XKB_KEY_Left ? n - 1 : 1)) % n);
			continue;
		}
		switch (look_type(typed, &g->typed_len, sym, mods, text)) {
		case LOOK_TYPED_SUBMIT:
			submit(g);
			break;
		case LOOK_TYPED_EDITED:
			g->hint[0] = '\0';
			break;
		default:
			break;
		}
	}
}

// ---- the people ----

// everyone side by side, the picked one bigger and lit. returns the field's
// glass lit, where it is in *fx
static void people(struct fui *ui, struct greet *g, double cx, double cy, double *fx,
		double *fy, double *lit) {
	int n = g->people.len;
	double left = cx - (n - 1) * SPACING / 2.0;
	for (int i = 0; i < n; i++) {
		bool picked = i == g->person;
		double grow = fui_glide(ui, fui_id("picked", (uint64_t)i), picked ? 1 : 0, 0.22);
		double size = AVATAR_OTHER + (AVATAR - AVATAR_OTHER) * grow;
		double x = left + i * SPACING;
		cairo_t *cr = fui_cairo(ui);
		cairo_push_group(cr);
		bool letter = look_avatar(ui, &g->people.person[i], x, cy, size);
		look_shadowed(ui, x, cy + AVATAR / 2.0 + 10, g->people.person[i].name,
			picked ? F_NAME : F_OTHER, LOOK_TEXT, 0.3);
		cairo_pop_group_to_source(cr);
		cairo_paint_with_alpha(cr, 0.6 + 0.4 * grow);
		if (letter) {
			glass(g, x - size / 2, cy - size / 2, size, size, 0xffffff26);
		}
		double r = size / 2;
		if (!picked && n > 1) {
			if (fui_hover(ui, x - r, cy - r, size, size)) {
				fui_cursor_pointer(ui);
			}
			if (fui_clicked(ui, x - r, cy - r, size, size)) {
				pick(g, i);
			}
		}
	}
	*fy = cy + AVATAR / 2.0 + 40;
	struct look_field field = {
		.typed_len = g->typed_len,
		.checking = greet_state() == GREET_CHECKING || g->leaving,
		.awake = true, // nothing to wake: it's always what's typed into
		.shaken_at = g->shaken_at,
		.typed_at = g->typed_at,
	};
	if (look_field(ui, &field, cx, *fy, fx, lit)) {
		submit(g);
	}
}

// what it logs into, under the field: a click for the next one
static void session(struct fui *ui, struct greet *g, double cx, double y) {
	const struct session *s = &g->people.session[g->session];
	bool more = g->people.sessions > 1;
	double tw = fui_text_width(ui, s->name, F_SMALL);
	double w = round(tw + (more ? 44 : 28)), h = 24, x = round(cx - w / 2);
	bool over = more && fui_hover(ui, x, y, w, h);
	glass(g, x, y, w, h, over ? 0xffffff2e : 0xffffff1a);
	fui_text(ui, x + 14, y + 5, s->name, F_SMALL, LOOK_TEXT, FUI_LEFT, 0);
	if (more) {
		// a small chevron: there's another
		cairo_t *cr = fui_cairo(ui);
		double ax = x + w - 16, ay = y + h / 2.0;
		cairo_new_path(cr);
		cairo_move_to(cr, ax - 3.5, ay - 1.5);
		cairo_line_to(cr, ax, ay + 2);
		cairo_line_to(cr, ax + 3.5, ay - 1.5);
		fui_color(ui, LOOK_TEXT_DIM);
		cairo_set_line_width(cr, 1.5);
		cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
		cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
		cairo_stroke(cr);
		if (over) {
			fui_cursor_pointer(ui);
		}
		if (fui_clicked(ui, x, y, w, h) && greet_state() != GREET_CHECKING) {
			g->session = (g->session + 1) % g->people.sessions;
		}
	}
}

// ---- sleep, restart, shut down ----

static void moon(cairo_t *cr, double cx, double cy) {
	cairo_push_group(cr);
	cairo_new_path(cr);
	cairo_arc(cr, cx, cy, 7.5, 0, 2 * M_PI);
	cairo_set_source_rgba(cr, 1, 1, 1, 1);
	cairo_fill(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_arc(cr, cx + 4.5, cy - 3.5, 6.5, 0, 2 * M_PI);
	cairo_fill(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	cairo_pop_group_to_source(cr);
	cairo_paint(cr);
}

// an arc open at the top: the power mark's ring, restart's with an arrow
static void ring(cairo_t *cr, double cx, double cy, bool arrow) {
	cairo_new_path(cr);
	double start = arrow ? -M_PI / 2 + 0.55 : -M_PI / 2 + 0.75;
	double end = arrow ? 3 * M_PI / 2 - 0.15 : 3 * M_PI / 2 - 0.75;
	cairo_arc(cr, cx, cy, 7.5, start, end);
	cairo_stroke(cr);
	if (arrow) {
		// the head where it ends, pointing on round
		double ex = cx + 7.5 * cos(end), ey = cy + 7.5 * sin(end);
		cairo_new_path(cr);
		cairo_move_to(cr, ex + 4.2, ey);
		cairo_line_to(cr, ex - 0.8, ey - 3.6);
		cairo_line_to(cr, ex - 0.8, ey + 3.6);
		cairo_close_path(cr);
		cairo_fill(cr);
	} else {
		cairo_new_path(cr);
		cairo_move_to(cr, cx, cy - 9.5);
		cairo_line_to(cr, cx, cy - 1.5);
		cairo_stroke(cr);
	}
}

static void power(struct fui *ui, struct greet *g, double cx, double y) {
	static const char *const names[POWER_COUNT] = {"Sleep", "Restart", "Shut Down"};
	int shown = 0;
	for (int i = 0; i < POWER_COUNT; i++) {
		shown += g->can[i];
	}
	double x = cx - (shown - 1) * 90 / 2.0;
	cairo_t *cr = fui_cairo(ui);
	for (int i = 0; i < POWER_COUNT; i++) {
		if (!g->can[i]) {
			continue;
		}
		bool over = fui_hover(ui, x - POWER_R, y - POWER_R, POWER_R * 2, POWER_R * 2);
		double lit = fui_glide(ui, fui_id("power lit", (uint64_t)i), over ? 1 : 0, 0.15);
		glass(g, x - POWER_R, y - POWER_R, POWER_R * 2, POWER_R * 2,
			0xffffff00 | (uint32_t)(0x1a + 0x1c * lit));
		fui_color(ui, LOOK_TEXT);
		cairo_set_line_width(cr, 1.8);
		cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
		if (i == POWER_SLEEP) {
			moon(cr, x, y);
		} else {
			ring(cr, x, y, i == POWER_RESTART);
		}
		look_shadowed(ui, x, y + POWER_R + 8, names[i], F_SMALL,
			0xffffff00 | (uint32_t)(0xb3 + 0x4c * lit), 0.3);
		if (over) {
			fui_cursor_pointer(ui);
		}
		if (fui_clicked(ui, x - POWER_R, y - POWER_R, POWER_R * 2, POWER_R * 2)
				&& !g->leaving) {
			power_do((enum power)i);
		}
		x += 90;
	}
}

static void frame(struct fui *ui, void *data) {
	struct greet *g = data;
	double w = fui_width(ui), h = fui_height(ui), now = fui_time(ui);
	if (!g->started) {
		g->started = true;
		g->active_at = now;
	}
	keys(ui, g);
	if (fui_button_held(ui) || fui_scrolled(ui) != 0) {
		g->active_at = now;
	}
	double px, py;
	fui_pointer(ui, &px, &py);
	if (px != g->mx || py != g->my) {
		g->mx = px;
		g->my = py;
		g->active_at = now;
	}
	switch (greet_state()) {
	case GREET_OK:
		if (!g->leaving) {
			g->leaving = true;
			people_picked(&g->people, g->person, g->session);
		}
		break;
	case GREET_FAILED:
		g->shaken_at = now;
		snprintf(g->hint, sizeof(g->hint), "%s", greet_said());
		greet_again();
		break;
	default:
		break;
	}
	bool awake = now - g->active_at < AWAKE || g->typed_len
		|| greet_state() == GREET_CHECKING;
	if (awake) {
		fui_after(ui, (int)((AWAKE - (now - g->active_at)) * 1000) + 50, minute, g);
	}
	// the picked one may have it off: then it's the wallpaper as it is
	bool blurred = !g->people.len || g->people.look[g->person].blur;
	double blur = fui_glide(ui, fui_id("blur", 0), blurred && (awake || g->leaving) ? 1 : 0,
		0.45);
	// in once it's up, out once someone's logged in: then it's gone, and
	// greetd starts their session
	double in = fui_glide_from(ui, fui_id("in", 0), 0, g->leaving ? 0 : 1, g->leaving
		? 0.3 : 0.5);
	if (g->leaving && in <= 0.001) {
		fui_quit(ui);
		return;
	}

	g->panes_len = 0;
	pane(g, (struct fui_pane){0, 0, (int)w, (int)h, 0, 40 * blur,
		0x00000000 | (uint32_t)(0x1a + 0x30 * blur), 1 + 0.25 * blur, 0, 0, 0});
	cairo_t *cr = fui_cairo(ui);
	cairo_push_group(cr);
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	char date[64], clock[16];
	strftime(date, sizeof(date), "%A, %-d %B", &tm);
	strftime(clock, sizeof(clock), "%H:%M", &tm);
	double top = round(h * 0.09);
	look_shadowed(ui, w / 2, top, date, F_DATE, 0xffffffe6, 0.25);
	look_shadowed(ui, w / 2, top + 22, clock, F_TIME, 0xffffffeb, 0.2);

	// the people a bit under the middle, all of it under them (the field,
	// what's said, the session) kept clear of the power buttons
	double under = AVATAR / 2.0 + 40 + LOOK_FIELD_H + 40 + 24 + 30;
	double ay = round(fmin(h * 0.62, h - 72 - POWER_R - under));
	if (g->people.len) {
		double fx, fy, lit;
		people(ui, g, w / 2, ay, &fx, &fy, &lit);
		glass(g, fx, fy, LOOK_FIELD_W, LOOK_FIELD_H,
			0xffffff00 | (uint32_t)(0x14 + 0x10 * fmin(lit, 1.6)));
		// why the last one didn't go, else what greetd's saying now
		const char *hint = *g->hint ? g->hint : greet_said();
		if (*hint) {
			look_shadowed(ui, w / 2, fy + LOOK_FIELD_H + 12, hint, F_HINT, LOOK_TEXT_DIM, 0.3);
		}
		session(ui, g, w / 2, fy + LOOK_FIELD_H + 40);
	} else {
		look_shadowed(ui, w / 2, ay, "No one here can log in", F_NAME, LOOK_TEXT_DIM, 0.3);
	}
	power(ui, g, w / 2, h - 72);
	cairo_pop_group_to_source(cr);
	cairo_paint_with_alpha(cr, in);
	// the glass fades with what's on it, the wallpaper's own stays
	for (int i = 1; i < g->panes_len; i++) {
		g->panes[i].fade = 1 - in;
	}
	fui_set_glass(ui, g->panes, g->panes_len);
	fui_after(ui, (int)(60 - tm.tm_sec) * 1000 + 50, minute, g);
}

int main(int argc, char **argv) {
	static struct greet g;
	mlock(typed, sizeof(typed));
	people_load(&g.people);
	g.person = g.people.last_person;
	g.session = g.people.last_session;
	power_check(g.can);
	if (g.people.len) {
		their_wallpaper(&g);
	}
	g.ui = fui_create(&(struct fui_config){
		.app_id = "flamegreet",
		.title = "Log In",
		.lock = true, // over everything, every key: nothing else is there yet
		.frame = frame,
		.data = &g,
	});
	if (!g.ui) {
		fprintf(stderr, "flamegreet: no wayland display (run it as flamewm -g flamegreet)\n");
		return 1;
	}
	g.mx = g.my = -1;
	int status = fui_run(g.ui);
	wipe(&g);
	fui_destroy(g.ui);
	return status;
}
