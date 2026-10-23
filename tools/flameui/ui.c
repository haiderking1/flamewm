#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cursor-shape-v1-client-protocol.h"
#include "internal.h"

// frame time: what the pointer and keys did since the last frame, values
// easing, and the controls. a control's look eases between states (on,
// hovered, pressed), nothing jumps

#define ACCENT 0xff7a2aff // flame
#define TEXT 0xffffffeb
#define TEXT_DIM 0xffffff99
#define FONT "Inter 10.5"
#define FONT_MEDIUM "Inter Medium 10.5"

static uint32_t with_alpha(uint32_t rgba, double a) {
	double base = (rgba & 0xff) / 255.0 * (a < 0 ? 0 : a > 1 ? 1 : a);
	return (rgba & 0xffffff00) | (uint32_t)lround(base * 255);
}

// a over b by t, both rgba
static uint32_t mix(uint32_t a, uint32_t b, double t) {
	uint32_t out = 0;
	for (int shift = 0; shift < 32; shift += 8) {
		double x = (a >> shift & 0xff), y = (b >> shift & 0xff);
		out |= (uint32_t)lround(x + (y - x) * t) << shift;
	}
	return out;
}

uint64_t fui_id(const char *s, uint64_t salt) {
	uint64_t h = 0xcbf29ce484222325ull ^ salt;
	for (; *s; s++) {
		h = (h ^ (unsigned char)*s) * 0x100000001b3ull;
	}
	return h ? h : 1;
}

// ---- frame ----

void fui_frame_begin(struct fui *ui, cairo_t *cr) {
	ui->cr = cr;
	ui->now = fui_now();
	if (!ui->last_frame) {
		ui->last_frame = ui->now;
	}
	ui->moving = false;
	ui->click_used = false;
	ui->right_used = false;
	ui->menu_press = false;
	ui->cursor_shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
	// a menu open takes the press before anything under it. outside it, it
	// closes: a left click goes nowhere else, a right one opens the next menu
	if (ui->menu && (ui->pressed || ui->right_pressed)) {
		const double *b = ui->menu_box;
		bool inside = ui->mx >= b[0] && ui->mx < b[0] + b[2] && ui->my >= b[1]
			&& ui->my < b[1] + b[3];
		if (inside) {
			ui->menu_press = ui->pressed;
			ui->click_used = ui->right_used = true;
		} else {
			ui->menu = 0;
			ui->click_used = ui->pressed;
		}
	}
	for (int i = 0; ui->menu && i < ui->keys_len; i++) {
		if (ui->keys[i].sym == XKB_KEY_Escape) {
			ui->menu = 0;
			ui->keys_len = 0; // that escape was the menu's
		}
	}
}

void fui_frame_end(struct fui *ui) {
	// input is spent
	ui->pressed = false;
	ui->right_pressed = false;
	ui->released = false;
	ui->scroll = 0;
	ui->keys_len = 0;
	fui_dnd_frame_end(ui); // a drop's files were the app's this frame
	if (!ui->button_down) {
		ui->active = 0;
	}
	ui->last_frame = ui->now;
	ui->cr = NULL;
}

cairo_t *fui_cairo(struct fui *ui) {
	return ui->cr;
}

int fui_width(const struct fui *ui) {
	return ui->width;
}

int fui_height(const struct fui *ui) {
	return ui->height;
}

double fui_time(const struct fui *ui) {
	return ui->now;
}

double fui_ease(struct fui *ui, uint64_t id, double target, double speed) {
	struct fui_ease_slot *slot = NULL;
	size_t at = (size_t)(id % FUI_EASES);
	for (size_t i = 0; i < FUI_EASES; i++) {
		struct fui_ease_slot *s = &ui->eases[(at + i) % FUI_EASES];
		if (!s->used || s->id == id) {
			slot = s;
			break;
		}
	}
	if (!slot) {
		return target;
	}
	if (!slot->used) {
		*slot = (struct fui_ease_slot){.id = id, .value = target, .target = target,
			.used = true};
		return target;
	}
	double dt = ui->now - ui->last_frame;
	dt = dt > 0.1 ? 0.1 : dt; // a long pause doesn't snap it
	slot->target = target;
	slot->value += (target - slot->value) * (1 - exp(-dt * speed));
	if (fabs(target - slot->value) < 0.002) {
		slot->value = target;
	} else {
		ui->moving = true;
	}
	return slot->value;
}

bool fui_hover(struct fui *ui, double x, double y, double w, double h) {
	return ui->pointer_in && ui->mx >= x && ui->mx < x + w && ui->my >= y && ui->my < y + h;
}

bool fui_clicked(struct fui *ui, double x, double y, double w, double h) {
	if (ui->pressed && !ui->click_used && fui_hover(ui, x, y, w, h)) {
		ui->click_used = true;
		return true;
	}
	return false;
}

// a focused field's clipboard keys: ctrl+v asks for the clipboard's text
// (it comes a frame or so later), ctrl+c copies what it says. true when k
// was one of them
static bool clipboard_key(struct fui *ui, uint64_t id, const struct fui_key *k,
		const char *text) {
	if (k->mods != FUI_CTRL) {
		return false;
	}
	if (xkb_keysym_to_lower(k->sym) == XKB_KEY_v) {
		fui_paste_text(ui, id);
		return true;
	}
	if (xkb_keysym_to_lower(k->sym) == XKB_KEY_c) {
		fui_copy_text(ui, text);
		return true;
	}
	return false;
}

// the clipboard's text, there for field id now: it goes in once
static void take_paste(struct fui *ui, uint64_t id, char *text, int size) {
	char *pasted = fui_pasted_text(ui, id);
	if (!pasted) {
		return;
	}
	size_t len = strlen(text);
	snprintf(text + len, (size_t)size - len, "%s", pasted);
	free(ui->pasted_text);
	ui->pasted_text = NULL;
}

// a key that types something: not a control character (tab, ctrl+f...)
static bool typable(const struct fui_key *k) {
	unsigned char c = (unsigned char)k->text[0];
	return c >= 0x20 && c != 0x7f && !(k->mods & (FUI_CTRL | FUI_ALT | FUI_SUPER));
}

int fui_click_count(const struct fui *ui) {
	return ui->click_count;
}

bool fui_right_clicked(struct fui *ui, double x, double y, double w, double h) {
	if (ui->right_pressed && !ui->right_used && fui_hover(ui, x, y, w, h)) {
		ui->right_used = true;
		return true;
	}
	return false;
}

void fui_pointer(const struct fui *ui, double *x, double *y) {
	*x = ui->mx;
	*y = ui->my;
}

bool fui_button_held(const struct fui *ui) {
	return ui->button_down;
}

bool fui_key_pressed(const struct fui *ui, uint32_t sym, unsigned mods) {
	if (ui->focus) {
		return false; // the field's
	}
	for (int i = 0; i < ui->keys_len; i++) {
		const struct fui_key *k = &ui->keys[i];
		if (xkb_keysym_to_lower(k->sym) == xkb_keysym_to_lower(sym) && k->mods == mods) {
			return true;
		}
	}
	return false;
}

bool fui_typing(const struct fui *ui) {
	return ui->focus != 0;
}

const char *fui_typed(struct fui *ui) {
	ui->typed[0] = '\0';
	for (int i = 0; !ui->focus && i < ui->keys_len; i++) {
		size_t len = strlen(ui->typed);
		if (typable(&ui->keys[i]) && len + strlen(ui->keys[i].text) < sizeof(ui->typed)) {
			strcat(ui->typed, ui->keys[i].text);
		}
	}
	return ui->typed;
}

void fui_focus(struct fui *ui, uint64_t id, const char *text) {
	ui->focus = id;
	snprintf(ui->focus_before, sizeof(ui->focus_before), "%s", text ? text : "");
	fui_redraw(ui);
}

double fui_scrolled(struct fui *ui) {
	return ui->scroll;
}

void fui_cursor_pointer(struct fui *ui) {
	ui->cursor_shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER;
}

void fui_cursor_hide(struct fui *ui) {
	ui->cursor_shape = FUI_CURSOR_HIDDEN;
}

// ---- drawing ----

void fui_color(struct fui *ui, uint32_t rgba) {
	cairo_set_source_rgba(ui->cr, (rgba >> 24 & 0xff) / 255.0, (rgba >> 16 & 0xff) / 255.0,
		(rgba >> 8 & 0xff) / 255.0, (rgba & 0xff) / 255.0);
}

void fui_rounded(struct fui *ui, double x, double y, double w, double h, double r) {
	cairo_t *cr = ui->cr;
	r = fmin(r, fmin(w, h) / 2);
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
	cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
}

void fui_fill_rounded(struct fui *ui, double x, double y, double w, double h, double r,
		uint32_t rgba) {
	fui_rounded(ui, x, y, w, h, r);
	fui_color(ui, rgba);
	cairo_fill(ui->cr);
}

static const PangoFontDescription *font(struct fui *ui, const char *name) {
	for (int i = 0; i < ui->fonts_len; i++) {
		if (!strcmp(ui->fonts[i].name, name)) {
			return ui->fonts[i].desc;
		}
	}
	PangoFontDescription *desc = pango_font_description_from_string(name);
	if (ui->fonts_len < FUI_FONTS) {
		snprintf(ui->fonts[ui->fonts_len].name, sizeof(ui->fonts[0].name), "%s", name);
		ui->fonts[ui->fonts_len++].desc = desc;
	}
	return desc;
}

void fui_fonts_free(struct fui *ui) {
	for (int i = 0; i < ui->fonts_len; i++) {
		pango_font_description_free(ui->fonts[i].desc);
	}
	ui->fonts_len = 0;
}

static PangoLayout *layout(struct fui *ui, const char *text, const char *font_name) {
	PangoLayout *l = pango_cairo_create_layout(ui->cr);
	pango_layout_set_font_description(l, font(ui, font_name));
	pango_layout_set_text(l, text, -1);
	return l;
}

double fui_text_width(struct fui *ui, const char *text, const char *font_name) {
	PangoLayout *l = layout(ui, text, font_name);
	int w, h;
	pango_layout_get_pixel_size(l, &w, &h);
	g_object_unref(l);
	return w;
}

double fui_text(struct fui *ui, double x, double y, const char *text, const char *font_name,
		uint32_t rgba, enum fui_align align, double max_width) {
	PangoLayout *l = layout(ui, text, font_name);
	if (max_width > 0) {
		pango_layout_set_width(l, (int)(max_width * PANGO_SCALE));
		pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
	}
	int w, h;
	pango_layout_get_pixel_size(l, &w, &h);
	double at = align == FUI_CENTER ? x - w / 2.0 : align == FUI_RIGHT ? x - w : x;
	fui_color(ui, rgba);
	cairo_move_to(ui->cr, at, y);
	pango_cairo_show_layout(ui->cr, l);
	g_object_unref(l);
	return w;
}

double fui_text_box(struct fui *ui, double x, double y, double width, int lines,
		const char *text, const char *font_name, uint32_t rgba, enum fui_align align) {
	PangoLayout *l = layout(ui, text, font_name);
	pango_layout_set_width(l, (int)(width * PANGO_SCALE));
	pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
	pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
	pango_layout_set_height(l, -lines);
	pango_layout_set_alignment(l, align == FUI_CENTER ? PANGO_ALIGN_CENTER
		: align == FUI_RIGHT ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT);
	int w, h;
	pango_layout_get_pixel_size(l, &w, &h);
	fui_color(ui, rgba);
	cairo_move_to(ui->cr, x, y);
	pango_cairo_show_layout(ui->cr, l);
	g_object_unref(l);
	return h;
}

double fui_paragraph(struct fui *ui, double x, double y, double width, const char *text,
		const char *font_name, uint32_t rgba) {
	PangoLayout *l = layout(ui, text, font_name);
	pango_layout_set_width(l, (int)(width * PANGO_SCALE));
	pango_layout_set_wrap(l, PANGO_WRAP_WORD);
	int w, h;
	pango_layout_get_pixel_size(l, &w, &h);
	fui_color(ui, rgba);
	cairo_move_to(ui->cr, x, y);
	pango_cairo_show_layout(ui->cr, l);
	g_object_unref(l);
	return h;
}

static void circle(struct fui *ui, double cx, double cy, double r, uint32_t rgba) {
	cairo_new_sub_path(ui->cr);
	cairo_arc(ui->cr, cx, cy, r, 0, 2 * M_PI);
	fui_color(ui, rgba);
	cairo_fill(ui->cr);
}

// ---- controls ----

bool fui_toggle(struct fui *ui, uint64_t id, double x, double y, bool *value, bool enabled) {
	const double w = 40, h = 24;
	bool hover = enabled && fui_hover(ui, x, y, w, h);
	bool changed = false;
	if (enabled && fui_clicked(ui, x, y, w, h)) {
		*value = !*value;
		changed = true;
	}
	if (hover) {
		fui_cursor_pointer(ui);
	}
	double on = fui_ease(ui, id, *value ? 1 : 0, 16);
	double lit = fui_ease(ui, id ^ 0x1, hover ? 1 : 0, 18);
	double fade = enabled ? 1 : 0.4;
	uint32_t track = mix(0xffffff2e, ACCENT, on);
	fui_fill_rounded(ui, x, y, w, h, h / 2, with_alpha(track, fade));
	if (lit > 0) {
		fui_fill_rounded(ui, x, y, w, h, h / 2, with_alpha(0xffffffff, 0.07 * lit));
	}
	double cx = x + 12 + 16 * on, cy = y + 12;
	circle(ui, cx, cy + 0.8, 10, with_alpha(0x00000000 | 0x40, fade));
	circle(ui, cx, cy, 10, with_alpha(0xffffffff, fade));
	return changed;
}

bool fui_slider(struct fui *ui, uint64_t id, double x, double y, double w, double *value,
		double min, double max, double step, bool enabled) {
	const double h = 22;
	bool hover = enabled && fui_hover(ui, x - 9, y, w + 18, h);
	if (enabled && ui->pressed && !ui->click_used && hover) {
		ui->click_used = true;
		ui->active = id;
	}
	bool dragging = ui->active == id && ui->button_down;
	if (ui->active == id) {
		double t = (ui->mx - x) / w;
		t = t < 0 ? 0 : t > 1 ? 1 : t;
		double v = min + t * (max - min);
		if (step > 0) {
			v = min + round((v - min) / step) * step;
		}
		*value = v < min ? min : v > max ? max : v;
	}
	bool changed = ui->active == id && ui->released;
	if (hover || dragging) {
		fui_cursor_pointer(ui);
	}
	double t = max > min ? (*value - min) / (max - min) : 0;
	double pos = fui_ease(ui, id, t, dragging ? 60 : 18);
	double grow = fui_ease(ui, id ^ 0x2, dragging ? 1 : hover ? 0.5 : 0, 18);
	double fade = enabled ? 1 : 0.4;
	fui_fill_rounded(ui, x, y + 9, w, 4, 2, with_alpha(0xffffff26, fade));
	fui_fill_rounded(ui, x, y + 9, w * pos, 4, 2, with_alpha(ACCENT, fade));
	double cx = x + w * pos, r = 9 + 1.5 * grow;
	circle(ui, cx, y + 11.8, r, with_alpha(0x00000040, fade));
	circle(ui, cx, y + 11, r, with_alpha(0xffffffff, fade));
	return changed;
}

double fui_segmented_width(struct fui *ui, const char *const *labels, int count) {
	double total = 4;
	for (int i = 0; i < count && i < 16; i++) {
		total += fui_text_width(ui, labels[i], FONT_MEDIUM) + 28;
	}
	return total;
}

bool fui_segmented(struct fui *ui, uint64_t id, double right, double y, const char *const *labels,
		int count, int *selected, bool enabled) {
	const double h = 28, pad = 14;
	double widths[16], total = 4;
	count = count > 16 ? 16 : count;
	for (int i = 0; i < count; i++) {
		widths[i] = fui_text_width(ui, labels[i], FONT_MEDIUM) + 2 * pad;
		total += widths[i];
	}
	double x = right - total;
	double fade = enabled ? 1 : 0.4;
	fui_fill_rounded(ui, x, y, total, h, 9, with_alpha(0xffffff14, fade));
	bool changed = false;
	double at = x + 2, sel_x = at, sel_w = 0;
	for (int i = 0; i < count; i++) {
		bool hover = enabled && fui_hover(ui, at, y, widths[i], h);
		if (hover) {
			fui_cursor_pointer(ui);
		}
		if (enabled && fui_clicked(ui, at, y, widths[i], h) && *selected != i) {
			*selected = i;
			changed = true;
		}
		if (i == *selected) {
			sel_x = at;
			sel_w = widths[i];
		}
		at += widths[i];
	}
	if (*selected >= 0) {
		double ex = fui_ease(ui, id, sel_x, 20), ew = fui_ease(ui, id ^ 0x3, sel_w, 20);
		fui_fill_rounded(ui, ex, y + 2, ew, h - 4, 7, with_alpha(0xffffff38, fade));
	}
	at = x + 2;
	for (int i = 0; i < count; i++) {
		uint32_t color = i == *selected ? TEXT : TEXT_DIM;
		fui_text(ui, at + widths[i] / 2, y + 6, labels[i], FONT_MEDIUM, with_alpha(color, fade),
			FUI_CENTER, 0);
		at += widths[i];
	}
	return changed;
}

bool fui_swatches(struct fui *ui, uint64_t id, double right, double y, const uint32_t *colors,
		int count, int *selected, bool enabled) {
	const double d = 22, gap = 10;
	double x = right - count * d - (count - 1) * gap;
	bool changed = false;
	double fade = enabled ? 1 : 0.4;
	for (int i = 0; i < count; i++) {
		double cx = x + i * (d + gap) + d / 2, cy = y + d / 2;
		bool hover = enabled && fui_hover(ui, cx - d / 2, y, d, d);
		if (hover) {
			fui_cursor_pointer(ui);
		}
		if (enabled && fui_clicked(ui, cx - d / 2, y, d, d) && *selected != i) {
			*selected = i;
			changed = true;
		}
		double ring = fui_ease(ui, id + (uint64_t)i * 7919, i == *selected ? 1 : hover ? 0.4 : 0,
			18);
		if (ring > 0.01) {
			cairo_new_sub_path(ui->cr);
			cairo_arc(ui->cr, cx, cy, d / 2 + 3.5, 0, 2 * M_PI);
			fui_color(ui, with_alpha(0xffffffff, ring * fade));
			cairo_set_line_width(ui->cr, 2);
			cairo_stroke(ui->cr);
		}
		circle(ui, cx, cy, d / 2, with_alpha(colors[i], fade));
		// a hairline: dark colors stay visible on dark glass
		cairo_new_sub_path(ui->cr);
		cairo_arc(ui->cr, cx, cy, d / 2 - 0.5, 0, 2 * M_PI);
		fui_color(ui, with_alpha(0xffffff30, fade));
		cairo_set_line_width(ui->cr, 1);
		cairo_stroke(ui->cr);
	}
	return changed;
}

// the last utf-8 character off
static void backspace(char *text) {
	size_t len = strlen(text);
	while (len > 0) {
		len--;
		if (((unsigned char)text[len] & 0xc0) != 0x80) {
			break;
		}
	}
	text[len] = '\0';
}


bool fui_field(struct fui *ui, uint64_t id, double x, double y, double w, char *text,
		int size, const char *placeholder, bool enabled) {
	const double h = 28;
	bool hover = enabled && fui_hover(ui, x, y, w, h);
	bool focused = ui->focus == id;
	bool committed = false;
	if (hover) {
		ui->cursor_shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT;
	}
	if (enabled && !focused && fui_clicked(ui, x, y, w, h)) {
		ui->focus = id;
		snprintf(ui->focus_before, sizeof(ui->focus_before), "%s", text);
		focused = true;
	} else if (focused && ui->pressed && !hover) {
		committed = true; // clicked away: kept
	}
	if (focused) {
		take_paste(ui, id, text, size);
		for (int i = 0; i < ui->keys_len && !committed; i++) {
			const struct fui_key *k = &ui->keys[i];
			if (clipboard_key(ui, id, k, text)) {
				continue;
			} else if (k->sym == XKB_KEY_Return || k->sym == XKB_KEY_KP_Enter) {
				committed = true;
			} else if (k->sym == XKB_KEY_Escape) {
				snprintf(text, (size_t)size, "%s", ui->focus_before);
				ui->focus = 0;
				focused = false;
			} else if (k->sym == XKB_KEY_BackSpace) {
				backspace(text);
			} else if (typable(k) && strlen(text) + strlen(k->text) < (size_t)size) {
				strcat(text, k->text);
			}
		}
	}
	bool changed = false;
	if (committed) {
		ui->focus = 0;
		focused = false;
		changed = strcmp(ui->focus_before, text) != 0;
	}
	double fade = enabled ? 1 : 0.4;
	double lit = fui_ease(ui, id, focused ? 1 : 0, 18);
	fui_fill_rounded(ui, x, y, w, h, 8, with_alpha(0xffffff12, fade));
	fui_rounded(ui, x + 0.5, y + 0.5, w - 1, h - 1, 8);
	fui_color(ui, with_alpha(mix(0xffffff24, ACCENT, lit), fade));
	cairo_set_line_width(ui->cr, 1 + lit);
	cairo_stroke(ui->cr);
	cairo_save(ui->cr);
	cairo_rectangle(ui->cr, x + 8, y, w - 16, h);
	cairo_clip(ui->cr);
	bool empty = !*text;
	double tw = fui_text(ui, x + 10, y + 6, empty ? placeholder : text, FONT,
		with_alpha(empty ? TEXT_DIM : TEXT, fade * (empty ? 0.7 : 1)), FUI_LEFT, 0);
	if (focused) {
		double cx = x + 10 + (empty ? 0 : tw) + 1;
		fui_fill_rounded(ui, cx, y + 6, 1.6, h - 12, 0.8, ACCENT);
	}
	cairo_restore(ui->cr);
	return changed;
}

bool fui_search(struct fui *ui, uint64_t id, double x, double y, double w, char *text,
		int size, bool grab) {
	const double h = 28;
	char before[256];
	snprintf(before, sizeof(before), "%s", text);
	bool hover = fui_hover(ui, x, y, w, h);
	bool over_clear = *text && fui_hover(ui, x + w - 26, y, 26, h);
	bool focused = ui->focus == id;
	if (over_clear) {
		fui_cursor_pointer(ui);
	} else if (hover) {
		ui->cursor_shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT;
	}
	if (*text && fui_clicked(ui, x + w - 26, y, 26, h)) {
		text[0] = '\0';
	} else if (!focused && fui_clicked(ui, x, y, w, h)) {
		ui->focus = id;
		focused = true;
	} else if (focused && ui->pressed && !hover) {
		ui->focus = 0;
		focused = false;
	}
	// typing with nothing else taking the keys: that's a search
	for (int i = 0; grab && i < ui->keys_len && !focused && !ui->focus; i++) {
		if (typable(&ui->keys[i])) {
			ui->focus = id;
			focused = true;
		}
	}
	if (focused) {
		take_paste(ui, id, text, size);
	}
	for (int i = 0; focused && i < ui->keys_len; i++) {
		const struct fui_key *k = &ui->keys[i];
		if (clipboard_key(ui, id, k, text)) {
			continue;
		} else if (k->sym == XKB_KEY_Escape) {
			// the search first, then the field
			if (*text) {
				text[0] = '\0';
			} else {
				ui->focus = 0;
				focused = false;
			}
		} else if (k->sym == XKB_KEY_BackSpace) {
			backspace(text);
		} else if (typable(k) && strlen(text) + strlen(k->text) < (size_t)size) {
			strcat(text, k->text);
		}
	}

	double lit = fui_ease(ui, id, focused ? 1 : 0, 18);
	fui_fill_rounded(ui, x, y, w, h, 8, 0xffffff14);
	fui_rounded(ui, x + 0.5, y + 0.5, w - 1, h - 1, 8);
	fui_color(ui, mix(0xffffff1a, ACCENT, lit));
	cairo_set_line_width(ui->cr, 1 + lit);
	cairo_stroke(ui->cr);
	// the magnifier
	fui_color(ui, TEXT_DIM);
	cairo_set_line_width(ui->cr, 1.5);
	cairo_set_line_cap(ui->cr, CAIRO_LINE_CAP_ROUND);
	cairo_arc(ui->cr, x + 14, y + 13, 4.5, 0, 2 * M_PI);
	cairo_stroke(ui->cr);
	cairo_move_to(ui->cr, x + 17.3, y + 16.3);
	cairo_line_to(ui->cr, x + 20.5, y + 19.5);
	cairo_stroke(ui->cr);
	cairo_save(ui->cr);
	cairo_rectangle(ui->cr, x + 26, y, w - 26 - (*text ? 26 : 8), h);
	cairo_clip(ui->cr);
	bool empty = !*text;
	// focused and empty: the caret first, the placeholder after it
	double tx = x + 27 + (focused && empty ? 4 : 0);
	double tw = fui_text(ui, tx, y + 6, empty ? "Search" : text, FONT,
		empty ? with_alpha(TEXT_DIM, 0.8) : TEXT, FUI_LEFT, 0);
	if (focused) {
		fui_fill_rounded(ui, x + 27 + (empty ? 0 : tw + 1), y + 6, 1.6, h - 12, 0.8, ACCENT);
	}
	cairo_restore(ui->cr);
	// clearing it
	if (*text) {
		double cx = x + w - 14, cy = y + h / 2;
		cairo_arc(ui->cr, cx, cy, 7, 0, 2 * M_PI);
		fui_color(ui, over_clear ? 0xffffff66 : 0xffffff3d);
		cairo_fill(ui->cr);
		fui_color(ui, 0x1c1c20ff);
		cairo_set_line_width(ui->cr, 1.4);
		cairo_move_to(ui->cr, cx - 2.6, cy - 2.6);
		cairo_line_to(ui->cr, cx + 2.6, cy + 2.6);
		cairo_move_to(ui->cr, cx + 2.6, cy - 2.6);
		cairo_line_to(ui->cr, cx - 2.6, cy + 2.6);
		cairo_stroke(ui->cr);
	}
	return strcmp(before, text) != 0;
}

bool fui_button(struct fui *ui, uint64_t id, double x, double y, const char *label) {
	double w = fui_text_width(ui, label, FONT_MEDIUM) + 28, h = 28;
	bool hover = fui_hover(ui, x, y, w, h);
	if (hover) {
		fui_cursor_pointer(ui);
	}
	bool clicked = fui_clicked(ui, x, y, w, h);
	double lit = fui_ease(ui, id, hover ? 1 : 0, 18);
	fui_fill_rounded(ui, x, y, w, h, 8, with_alpha(0xffffffff, 0.10 + 0.08 * lit));
	fui_text(ui, x + w / 2, y + 6, label, FONT_MEDIUM, TEXT, FUI_CENTER, 0);
	return clicked;
}

// ---- menus ----

#define MENU_ITEM_H 26
#define MENU_LINE_H 9
#define MENU_PAD 5

void fui_menu_open(struct fui *ui, uint64_t id, double x, double y) {
	ui->menu = id;
	ui->menu_x = x;
	ui->menu_y = y;
	ui->menu_box[2] = ui->menu_box[3] = 0;
	fui_redraw(ui);
}

bool fui_menu_is_open(const struct fui *ui, uint64_t id) {
	return ui->menu == id;
}

void fui_menu_close(struct fui *ui) {
	ui->menu = 0;
	fui_redraw(ui);
}

int fui_menu(struct fui *ui, uint64_t id, const struct fui_menu_item *items, int count) {
	if (ui->menu != id) {
		return -1;
	}
	double w = 160, h = 2 * MENU_PAD;
	for (int i = 0; i < count; i++) {
		if (!items[i].label) {
			h += MENU_LINE_H;
			continue;
		}
		h += MENU_ITEM_H;
		double tw = fui_text_width(ui, items[i].label, FONT) + 40;
		if (items[i].shortcut) {
			tw += fui_text_width(ui, items[i].shortcut, FONT) + 24;
		}
		w = tw > w ? tw : w;
	}
	// in the window, the way the pointer was going if it fits
	double x = ui->menu_x, y = ui->menu_y;
	x = x + w > ui->width - 4 ? x - w : x;
	y = y + h > ui->height - 4 ? y - h : y;
	x = x < 4 ? 4 : x;
	y = y < 4 ? 4 : y;
	ui->menu_box[0] = x;
	ui->menu_box[1] = y;
	ui->menu_box[2] = w;
	ui->menu_box[3] = h;

	double shown = fui_ease(ui, id ^ 0x6d656e75, 1, 30);
	cairo_push_group(ui->cr);
	// a shadow, then the menu
	for (int i = 3; i > 0; i--) {
		fui_fill_rounded(ui, x - i, y - i + 3, w + 2 * i, h + 2 * i, 10 + i, 0x00000014);
	}
	fui_fill_rounded(ui, x, y, w, h, 10, 0x2b2b31f7);
	fui_rounded(ui, x + 0.5, y + 0.5, w - 1, h - 1, 10);
	fui_color(ui, 0xffffff24);
	cairo_set_line_width(ui->cr, 1);
	cairo_stroke(ui->cr);
	int picked = -1;
	double iy = y + MENU_PAD;
	for (int i = 0; i < count; i++) {
		if (!items[i].label) {
			fui_fill_rounded(ui, x + 10, iy + MENU_LINE_H / 2.0, w - 20, 1, 0, 0xffffff1a);
			iy += MENU_LINE_H;
			continue;
		}
		bool over = !items[i].disabled && fui_hover(ui, x + MENU_PAD, iy, w - 2 * MENU_PAD,
			MENU_ITEM_H);
		if (over) {
			fui_fill_rounded(ui, x + MENU_PAD, iy, w - 2 * MENU_PAD, MENU_ITEM_H, 6, ACCENT);
			if (ui->menu_press) {
				picked = i;
			}
		}
		uint32_t color = items[i].disabled ? with_alpha(TEXT, 0.35) : TEXT;
		fui_text(ui, x + 14, iy + 4, items[i].label, FONT, color, FUI_LEFT, 0);
		if (items[i].shortcut) {
			fui_text(ui, x + w - 14, iy + 4, items[i].shortcut, FONT,
				over ? TEXT : with_alpha(TEXT, 0.45), FUI_RIGHT, 0);
		}
		iy += MENU_ITEM_H;
	}
	cairo_pop_group_to_source(ui->cr);
	cairo_paint_with_alpha(ui->cr, shown);
	if (picked >= 0) {
		ui->menu = 0;
		fui_redraw(ui);
	}
	return picked;
}
