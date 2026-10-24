#include "look.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#define F_FIELD "Inter 11"

static void minute(struct fui *ui, void *data) {
	fui_redraw(ui);
}

void look_person(struct look_person *person, const struct passwd *pw) {
	const char *user = pw ? pw->pw_name : "";
	snprintf(person->user, sizeof(person->user), "%s", user);
	char gecos[128] = "";
	if (pw && pw->pw_gecos) {
		snprintf(gecos, sizeof(gecos), "%s", pw->pw_gecos);
		gecos[strcspn(gecos, ",")] = '\0';
	}
	snprintf(person->name, sizeof(person->name), "%s", *gecos ? gecos : user);
	if (!*gecos && person->name[0] >= 'a' && person->name[0] <= 'z') {
		person->name[0] = (char)(person->name[0] - 'a' + 'A');
	}
	// their picture, where desktops keep it
	person->avatar[0] = '\0';
	const char *home = pw ? pw->pw_dir : "";
	char candidates[3][4096];
	snprintf(candidates[0], sizeof(candidates[0]), "%s/.face", home ? home : "");
	snprintf(candidates[1], sizeof(candidates[1]), "%s/.face.icon", home ? home : "");
	snprintf(candidates[2], sizeof(candidates[2]), "/var/lib/AccountsService/icons/%s", user);
	for (int i = 0; i < 3; i++) {
		if (access(candidates[i], R_OK) == 0) {
			snprintf(person->avatar, sizeof(person->avatar), "%s", candidates[i]);
			break;
		}
	}
}

void look_shadowed(struct fui *ui, double x, double y, const char *text, const char *font,
		uint32_t color, double shadow) {
	fui_text(ui, x, y + 1.5, text, font, 0x00000000 | (uint32_t)(0xff * shadow), FUI_CENTER, 0);
	fui_text(ui, x, y, text, font, color, FUI_CENTER, 0);
}

bool look_avatar(struct fui *ui, const struct look_person *person, double cx, double cy,
		double size) {
	cairo_t *cr = fui_cairo(ui);
	double r = size / 2.0;
	cairo_surface_t *picture = *person->avatar
		? fui_thumb(ui, person->avatar, (int)size * 2, (int)size * 2, NULL) : NULL;
	cairo_new_path(cr);
	cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
	if (picture) {
		cairo_save(cr);
		cairo_clip(cr);
		int pw = cairo_image_surface_get_width(picture);
		cairo_translate(cr, cx - r, cy - r);
		cairo_scale(cr, size / (double)pw, size / (double)pw);
		cairo_set_source_surface(cr, picture, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
		cairo_paint(cr);
		cairo_restore(cr);
		return false;
	}
	cairo_new_path(cr);
	char letter[8] = "?";
	size_t len = 1;
	while (person->name[len] && ((unsigned char)person->name[len] & 0xc0) == 0x80 && len < 4) {
		len++;
	}
	if (*person->name) {
		memcpy(letter, person->name, len);
		letter[len] = '\0';
	}
	char font[32];
	snprintf(font, sizeof(font), "Inter Semi-Bold %d", (int)round(size / 3));
	fui_text(ui, cx, cy - size * 0.236, letter, font, LOOK_TEXT, FUI_CENTER, 0);
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

bool look_field(struct fui *ui, const struct look_field *f, double cx, double y, double *at,
		double *lit_out) {
	cairo_t *cr = fui_cairo(ui);
	// a wrong one shakes it: quick and dying away
	double t = fui_time(ui) - f->shaken_at;
	double shake = t < 0.6 ? sin(t * 42) * 11 * exp(-t * 6.5) : 0;
	if (t < 0.6) {
		fui_redraw(ui);
	}
	double x = round(cx - LOOK_FIELD_W / 2.0 + shake);
	double now = fui_time(ui);
	// lit while it's what's typed into, a little more while it's pressed
	bool pressed = fui_button_held(ui) && fui_hover(ui, x, y, LOOK_FIELD_W, LOOK_FIELD_H);
	double lit = fui_glide(ui, fui_id("field lit", 0), f->awake ? (pressed ? 1.6 : 1) : 0, 0.2);
	field_ring(ui, x, y, LOOK_FIELD_W, LOOK_FIELD_H, lit);
	*at = x;
	*lit_out = lit;
	// the caret: where what's typed goes, lit while typing, blinking after
	if (f->awake && !f->checking) {
		double since = now - f->typed_at;
		if (since < 0.6 || fmod(since, 1.06) < 0.53) {
			int shown = f->typed_len < 18 ? f->typed_len : 18;
			double caret = shown ? x + 18 + shown * 11 - 3 : x + 16;
			fui_fill_rounded(ui, caret, y + 9, 2, LOOK_FIELD_H - 18, 1, LOOK_TEXT);
		}
		fui_after(ui, 530, minute, NULL);
	}
	if (!f->typed_len && !f->checking) {
		fui_text(ui, x + LOOK_FIELD_W / 2.0, y + 9, "Enter Password", F_FIELD, LOOK_TEXT_DIM,
			FUI_CENTER, 0);
	}
	// a dot each, as many as fit
	int dots = f->typed_len < 18 ? f->typed_len : 18;
	for (int i = 0; i < dots; i++) {
		cairo_new_path(cr);
		cairo_arc(cr, x + 18 + i * 11, y + LOOK_FIELD_H / 2.0, 3.2, 0, 2 * M_PI);
		fui_color(ui, LOOK_TEXT);
		cairo_fill(cr);
	}
	// at its right: caps lock, the arrow that goes, the wheel while it checks
	double bx = x + LOOK_FIELD_W - LOOK_FIELD_H / 2.0, by = y + LOOK_FIELD_H / 2.0;
	if (fui_caps_lock(ui) && !f->checking) {
		fui_color(ui, LOOK_TEXT_DIM);
		caps_mark(cr, bx - (f->typed_len ? 28 : 4), by);
	}
	if (f->checking) {
		double a = fui_time(ui) * 7;
		cairo_new_path(cr);
		cairo_arc(cr, bx, by, 8, a, a + M_PI * 1.4);
		fui_color(ui, LOOK_TEXT);
		cairo_set_line_width(cr, 2);
		cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
		cairo_stroke(cr);
		fui_redraw(ui); // it turns while it's at it
		return false;
	}
	if (!f->typed_len) {
		return false;
	}
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
	fui_color(ui, LOOK_TEXT);
	cairo_set_line_width(cr, 1.8);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	cairo_stroke(cr);
	if (over) {
		fui_cursor_pointer(ui);
	}
	return fui_clicked(ui, bx - 12, by - 12, 24, 24);
}

enum look_typed look_type(char *typed, int *len_chars, uint32_t sym, unsigned mods,
		const char *text) {
	bool plain = !(mods & (FUI_CTRL | FUI_ALT | FUI_SUPER));
	bool ctrl = mods == FUI_CTRL;
	size_t len = strlen(typed);
	if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
		return LOOK_TYPED_SUBMIT;
	}
	if (sym == XKB_KEY_Escape || (ctrl && (sym == XKB_KEY_u || sym == XKB_KEY_U
			|| sym == XKB_KEY_BackSpace))) {
		explicit_bzero(typed, LOOK_TYPED_MAX);
		*len_chars = 0;
		return LOOK_TYPED_EDITED;
	}
	if (sym == XKB_KEY_BackSpace && len) {
		while (len && ((unsigned char)typed[len - 1] & 0xc0) == 0x80) {
			len--;
		}
		typed[len - (len > 0)] = '\0';
		*len_chars -= *len_chars > 0;
		return LOOK_TYPED_EDITED;
	}
	if (plain && *text && len + strlen(text) < LOOK_TYPED_MAX) {
		memcpy(typed + len, text, strlen(text) + 1);
		(*len_chars)++;
		return LOOK_TYPED_EDITED;
	}
	return LOOK_TYPED_NOTHING;
}
