// flamelock: the session locked till the user's password is typed. the
// wallpaper, the time big over it, the user's picture and name and a field
// for the password under them; the wallpaper softens while someone's at it.
// a wrong one shakes the field. super+l

#include <math.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "../flameui/flameui.h"
#include "auth.h"
#include "look.h"

#define F_DATE "Inter Semi-Bold 20"
#define F_TIME "Inter Semi-Bold 104"
#define F_NAME "Inter Semi-Bold 14"
#define AVATAR 72
#define AWAKE 10.0 // seconds it stays soft after the last key or move

struct lock {
	struct fui *ui;
	struct look_person who;
	int typed_len; // characters, for the dots
	double active_at; // the last key, click or move of the mouse
	double typed_at; // the last key: the caret stays lit a moment after
	bool started;
	double shaken_at; // a wrong one: when it shook
	double mx, my;
	bool unlocking;
};

// what's typed: locked in memory, wiped as soon as it's not needed
static char typed[LOOK_TYPED_MAX];

static void wipe(struct lock *l) {
	explicit_bzero(typed, sizeof(typed));
	l->typed_len = 0;
}

static void minute(struct fui *ui, void *data) {
	fui_redraw(ui);
}

static void submit(struct lock *l) {
	if (l->typed_len && auth_state() == AUTH_IDLE) {
		auth_check(l->ui, typed);
		wipe(l); // the checker has its own copy, wiped after too
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
		if (look_type(typed, &l->typed_len, sym, mods, text) == LOOK_TYPED_SUBMIT) {
			submit(l);
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
	look_shadowed(ui, w / 2, top, date, F_DATE, 0xffffffe6, 0.25);
	look_shadowed(ui, w / 2, top + 22, clock, F_TIME, 0xffffffeb, 0.2);
	// the user, at the bottom
	double ay = h - 190;
	bool letter = look_avatar(ui, &l->who, w / 2, ay, AVATAR);
	look_shadowed(ui, w / 2, ay + AVATAR / 2.0 + 10, l->who.name, F_NAME, LOOK_TEXT, 0.3);
	double fx, lit, fy = ay + AVATAR / 2.0 + 40;
	struct look_field field = {
		.typed_len = l->typed_len,
		.checking = auth_state() == AUTH_CHECKING,
		.awake = now - l->active_at < AWAKE || l->typed_len,
		.shaken_at = l->shaken_at,
		.typed_at = l->typed_at,
	};
	if (look_field(ui, &field, w / 2, fy, &fx, &lit)) {
		submit(l);
	}
	cairo_pop_group_to_source(cr);
	cairo_paint_with_alpha(cr, in);
	// flame's glass: all of the wallpaper softened and dimmed by how blurred,
	// and the field's, lighter while it's what's typed into
	struct fui_pane panes[] = {
		{0, 0, (int)w, (int)h, 0, 40 * blur, 0x00000000 | (uint32_t)(0x1a + 0x30 * blur),
			1 + 0.25 * blur, 0, 0, 0},
		{(int)fx, (int)fy, LOOK_FIELD_W, LOOK_FIELD_H, LOOK_FIELD_H / 2.0, 12,
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
	look_person(&l.who, getpwuid(getuid()));
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
