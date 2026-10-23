// flamelock: the session locked till the user's password is typed. the
// wallpaper, the time big over it, the user's picture and name and a field
// for the password under them; the wallpaper softens while someone's at it.
// a wrong one shakes the field. super+l

#include <math.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "../flameui/flameui.h"
#include "auth.h"

#define TEXT 0xffffffff
#define TEXT_DIM 0xffffffb3
#define F_DATE "Inter Semi-Bold 20"
#define F_TIME "Inter Semi-Bold 104"
#define F_NAME "Inter Semi-Bold 14"
#define F_FIELD "Inter 11"
#define F_HINT "Inter Medium 10"
#define FIELD_W 240
#define FIELD_H 34
#define AVATAR 72
#define AWAKE 10.0 // seconds it stays soft after the last key or move
#define TYPED_MAX 1024

struct lock {
	struct fui *ui;
	char name[128];
	char avatar[4096]; // a picture of the user, "" for none
	int typed_len; // characters, for the dots
	double active_at; // the last key, click or move of the mouse
	double typed_at; // the last key: the caret stays lit a moment after
	bool started;
	double shaken_at; // a wrong one: when it shook
	double mx, my;
	bool unlocking;
};

// what's typed: locked in memory, wiped as soon as it's not needed
static char typed[TYPED_MAX];

static void wipe(struct lock *l) {
	explicit_bzero(typed, sizeof(typed));
	l->typed_len = 0;
}

// ---- who ----

static void who(struct lock *l) {
	struct passwd *pw = getpwuid(getuid());
	const char *user = pw ? pw->pw_name : "";
	// the full name, else the user name with a capital
	char gecos[128] = "";
	if (pw && pw->pw_gecos) {
		snprintf(gecos, sizeof(gecos), "%s", pw->pw_gecos);
		gecos[strcspn(gecos, ",")] = '\0';
	}
	snprintf(l->name, sizeof(l->name), "%s", *gecos ? gecos : user);
	if (!*gecos && l->name[0] >= 'a' && l->name[0] <= 'z') {
		l->name[0] = (char)(l->name[0] - 'a' + 'A');
	}
	// the user's picture, where desktops keep it
	const char *home = pw ? pw->pw_dir : getenv("HOME");
	char candidates[3][4096];
	snprintf(candidates[0], sizeof(candidates[0]), "%s/.face", home ? home : "");
	snprintf(candidates[1], sizeof(candidates[1]), "%s/.face.icon", home ? home : "");
	snprintf(candidates[2], sizeof(candidates[2]), "/var/lib/AccountsService/icons/%s", user);
	for (int i = 0; i < 3; i++) {
		if (access(candidates[i], R_OK) == 0) {
			snprintf(l->avatar, sizeof(l->avatar), "%s", candidates[i]);
			break;
		}
	}
}

static void minute(struct fui *ui, void *data) {
	fui_redraw(ui);
}

// ---- drawing ----

static void shadowed(struct fui *ui, double x, double y, const char *text, const char *font,
		uint32_t color, double shadow) {
	fui_text(ui, x, y + 1.5, text, font, 0x00000000 | (uint32_t)(0xff * shadow), FUI_CENTER, 0);
	fui_text(ui, x, y, text, font, color, FUI_CENTER, 0);
}

// the user's picture in a circle, else the first letter of their name on
// glass: true for the glass
static bool avatar(struct fui *ui, struct lock *l, double cx, double cy) {
	cairo_t *cr = fui_cairo(ui);
	double r = AVATAR / 2.0;
	cairo_surface_t *picture = *l->avatar
		? fui_thumb(ui, l->avatar, AVATAR * 2, AVATAR * 2, NULL) : NULL;
	cairo_new_path(cr);
	cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
	if (picture) {
		cairo_save(cr);
		cairo_clip(cr);
		int pw = cairo_image_surface_get_width(picture);
		cairo_translate(cr, cx - r, cy - r);
		cairo_scale(cr, AVATAR / (double)pw, AVATAR / (double)pw);
		cairo_set_source_surface(cr, picture, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
		cairo_paint(cr);
		cairo_restore(cr);
		return false;
	}
	cairo_new_path(cr);
	char letter[8] = "?";
	size_t len = 1;
	while (l->name[len] && ((unsigned char)l->name[len] & 0xc0) == 0x80 && len < 4) {
		len++;
	}
	if (*l->name) {
		memcpy(letter, l->name, len);
		letter[len] = '\0';
	}
	fui_text(ui, cx, cy - 17, letter, "Inter Semi-Bold 24", TEXT, FUI_CENTER, 0);
	return true;
}

// the field's ring while it's what's typed into (its glass is flame's)
static void field_ring(struct fui *ui, double x, double y, double w, double h, double lit) {
	cairo_t *cr = fui_cairo(ui);
	fui_rounded(ui, x + 0.5, y + 0.5, w - 1, h - 1, h / 2 - 0.5);
	fui_color(ui, 0xffffff00 | (uint32_t)(0x26 + 0x33 * fmin(lit, 1.6)));
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
	if (lit > 0.01) {
		fui_rounded(ui, x - 2.5, y - 2.5, w + 5, h + 5, h / 2 + 2.5);
		fui_color(ui, 0xffffff00 | (uint32_t)(0x2e * fmin(lit, 1.6)));
		cairo_set_line_width(cr, 3);
		cairo_stroke(cr);
	}
}

// caps lock: an arrow up over a bar
static void caps_mark(cairo_t *cr, double cx, double cy) {
	cairo_new_path(cr);
	cairo_move_to(cr, cx, cy - 6);
	cairo_line_to(cr, cx + 5.5, cy);
	cairo_line_to(cr, cx + 2.5, cy);
	cairo_line_to(cr, cx + 2.5, cy + 3);
	cairo_line_to(cr, cx - 2.5, cy + 3);
	cairo_line_to(cr, cx - 2.5, cy);
	cairo_line_to(cr, cx - 5.5, cy);
	cairo_close_path(cr);
	cairo_rectangle(cr, cx - 2.5, cy + 4.5, 5, 1.6);
	cairo_fill(cr);
}

static void submit(struct lock *l) {
	if (l->typed_len && auth_state() == AUTH_IDLE) {
		auth_check(l->ui, typed);
		wipe(l); // the checker has its own copy, wiped after too
	}
}

// drawn at cx, y: where it is (it shakes) and how lit, for its glass
static void field(struct fui *ui, struct lock *l, double cx, double y, double *at,
		double *lit_out) {
	cairo_t *cr = fui_cairo(ui);
	// a wrong one shakes it: quick and dying away
	double t = fui_time(ui) - l->shaken_at;
	double shake = t < 0.6 ? sin(t * 42) * 11 * exp(-t * 6.5) : 0;
	if (t < 0.6) {
		fui_redraw(ui);
	}
	double x = round(cx - FIELD_W / 2.0 + shake);
	double now = fui_time(ui);
	bool checking = auth_state() == AUTH_CHECKING;
	bool awake = now - l->active_at < AWAKE || l->typed_len;
	// lit while it's what's typed into, a little more while it's pressed
	bool pressed = fui_button_held(ui) && fui_hover(ui, x, y, FIELD_W, FIELD_H);
	double lit = fui_glide(ui, fui_id("field lit", 0), awake ? (pressed ? 1.6 : 1) : 0, 0.2);
	field_ring(ui, x, y, FIELD_W, FIELD_H, lit);
	*at = x;
	*lit_out = lit;
	// the caret: where what's typed goes, lit while typing, blinking after
	if (awake && !checking) {
		double since = now - l->typed_at;
		if (since < 0.6 || fmod(since, 1.06) < 0.53) {
			int shown = l->typed_len < 18 ? l->typed_len : 18;
			double caret = shown ? x + 18 + shown * 11 - 3 : x + 16;
			fui_fill_rounded(ui, caret, y + 9, 2, FIELD_H - 18, 1, TEXT);
		}
		fui_after(ui, 530, minute, l);
	}
	if (!l->typed_len && !checking) {
		fui_text(ui, x + FIELD_W / 2.0, y + 9, "Enter Password", F_FIELD, TEXT_DIM, FUI_CENTER,
			0);
	}
	// a dot each, as many as fit
	int dots = l->typed_len < 18 ? l->typed_len : 18;
	for (int i = 0; i < dots; i++) {
		cairo_new_path(cr);
		cairo_arc(cr, x + 18 + i * 11, y + FIELD_H / 2.0, 3.2, 0, 2 * M_PI);
		fui_color(ui, TEXT);
		cairo_fill(cr);
	}
	// at its right: caps lock, the arrow that goes, the wheel while it checks
	double bx = x + FIELD_W - FIELD_H / 2.0, by = y + FIELD_H / 2.0;
	if (fui_caps_lock(ui) && !checking) {
		fui_color(ui, TEXT_DIM);
		caps_mark(cr, bx - (l->typed_len ? 28 : 4), by);
	}
	if (checking) {
		double a = fui_time(ui) * 7;
		cairo_new_path(cr);
		cairo_arc(cr, bx, by, 8, a, a + M_PI * 1.4);
		fui_color(ui, TEXT);
		cairo_set_line_width(cr, 2);
		cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
		cairo_stroke(cr);
		fui_redraw(ui); // it turns while it's at it
	} else if (l->typed_len) {
		bool over = fui_hover(ui, bx - 12, by - 12, 24, 24);
		cairo_new_path(cr);
		cairo_arc(cr, bx, by, 11, 0, 2 * M_PI);
		fui_color(ui, over ? 0xffffff66 : 0xffffff40);
		cairo_fill(cr);
		cairo_move_to(cr, bx - 4.5, by);
		cairo_line_to(cr, bx + 4, by);
		cairo_move_to(cr, bx + 0.5, by - 3.5);
		cairo_line_to(cr, bx + 4, by);
		cairo_line_to(cr, bx + 0.5, by + 3.5);
		fui_color(ui, TEXT);
		cairo_set_line_width(cr, 1.8);
		cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
		cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
		cairo_stroke(cr);
		if (over) {
			fui_cursor_pointer(ui);
		}
		if (fui_clicked(ui, bx - 12, by - 12, 24, 24)) {
			submit(l);
		}
	}
}

// ---- keys ----

static void keys(struct fui *ui, struct lock *l) {
	uint32_t sym;
	unsigned mods;
	const char *text;
	for (int i = 0; fui_key_at(ui, i, &sym, &mods, &text); i++) {
		l->active_at = l->typed_at = fui_time(ui);
		if (auth_state() != AUTH_IDLE || l->unlocking) {
			continue; // checking: nothing typed goes anywhere
		}
		bool plain = !(mods & (FUI_CTRL | FUI_ALT | FUI_SUPER));
		bool ctrl = mods == FUI_CTRL;
		size_t len = strlen(typed);
		if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
			submit(l);
		} else if (sym == XKB_KEY_Escape || (ctrl && (sym == XKB_KEY_u || sym == XKB_KEY_U
				|| sym == XKB_KEY_BackSpace))) {
			wipe(l);
		} else if (sym == XKB_KEY_BackSpace && len) {
			while (len && ((unsigned char)typed[len - 1] & 0xc0) == 0x80) {
				len--;
			}
			typed[len - (len > 0)] = '\0';
			l->typed_len -= l->typed_len > 0;
		} else if (plain && *text && len + strlen(text) < TYPED_MAX) {
			memcpy(typed + len, text, strlen(text) + 1);
			l->typed_len++;
		}
	}
}

static void frame(struct fui *ui, void *data) {
	struct lock *l = data;
	double w = fui_width(ui), h = fui_height(ui), now = fui_time(ui);
	if (!l->started) {
		// just locked: whoever locked it's right there, the field's ready
		l->started = true;
		l->active_at = now;
	}
	keys(ui, l);
	if (fui_button_held(ui) || fui_scrolled(ui) != 0) {
		l->active_at = now; // a click or a scroll wakes it as much as a move
	}
	double px, py;
	fui_pointer(ui, &px, &py);
	if (px != l->mx || py != l->my) {
		l->mx = px;
		l->my = py;
		l->active_at = now;
	}
	switch (auth_state()) {
	case AUTH_OK:
		l->unlocking = true;
		break;
	case AUTH_FAILED:
		l->shaken_at = now;
		auth_again();
		break;
	default:
		break;
	}
	bool awake = now - l->active_at < AWAKE || l->typed_len || auth_state() == AUTH_CHECKING;
	if (awake) {
		fui_after(ui, (int)((AWAKE - (now - l->active_at)) * 1000) + 50, minute, l);
	}
	double blur = fui_glide(ui, fui_id("blur", 0), awake ? 1 : 0, 0.45);
	// in once it's up, out once it's unlocked: then it's gone
	double in = fui_glide_from(ui, fui_id("in", 0), 0, l->unlocking ? 0 : 1, l->unlocking
		? 0.25 : 0.5);
	if (l->unlocking && in <= 0.001) {
		fui_unlock(ui);
		return;
	}

	cairo_t *cr = fui_cairo(ui);
	cairo_push_group(cr);
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	char date[64], clock[16];
	strftime(date, sizeof(date), "%A, %-d %B", &tm);
	strftime(clock, sizeof(clock), "%H:%M", &tm);
	double top = round(h * 0.09);
	shadowed(ui, w / 2, top, date, F_DATE, 0xffffffe6, 0.25);
	shadowed(ui, w / 2, top + 22, clock, F_TIME, 0xffffffeb, 0.2);
	// the user, at the bottom
	double ay = h - 190;
	bool letter = avatar(ui, l, w / 2, ay);
	shadowed(ui, w / 2, ay + AVATAR / 2.0 + 10, l->name, F_NAME, TEXT, 0.3);
	double fx, lit, fy = ay + AVATAR / 2.0 + 40;
	field(ui, l, w / 2, fy, &fx, &lit);
	cairo_pop_group_to_source(cr);
	cairo_paint_with_alpha(cr, in);
	// flame's glass: all of the wallpaper softened and dimmed by how blurred,
	// and the field's, lighter while it's what's typed into
	struct fui_pane panes[] = {
		{0, 0, (int)w, (int)h, 0, 40 * blur, 0x00000000 | (uint32_t)(0x1a + 0x30 * blur),
			1 + 0.25 * blur, 0, 0, 0},
		{(int)fx, (int)fy, FIELD_W, FIELD_H, FIELD_H / 2.0, 12,
			0xffffff00 | (uint32_t)(0x14 + 0x10 * fmin(lit, 1.6)), 1.8, 0.22, 18, 0},
		{(int)(w / 2 - AVATAR / 2.0), (int)(ay - AVATAR / 2.0), AVATAR, AVATAR, AVATAR / 2.0,
			12, 0xffffff26, 1.8, 0.22, 18, 0},
	};
	fui_set_glass(ui, panes, letter ? 3 : 2);
	fui_after(ui, (int)(60 - tm.tm_sec) * 1000 + 50, minute, l);
}

int main(int argc, char **argv) {
	static struct lock l;
	mlock(typed, sizeof(typed));
	who(&l);
	l.ui = fui_create(&(struct fui_config){
		.app_id = "flamelock",
		.title = "Lock",
		.lock = true,
		.frame = frame,
		.data = &l,
	});
	if (!l.ui) {
		fprintf(stderr, "flamelock: can't lock (no wayland display, or flame won't)\n");
		return 1;
	}
	l.mx = l.my = -1; // where fui says the pointer is till it's seen
	int status = fui_run(l.ui);
	wipe(&l);
	fui_destroy(l.ui);
	return status;
}
