#include <stdlib.h>
#include <string.h>

#include "internal.h"

// a line of text being edited: the caret and what's selected (bytes into
// it, kept for one line at a time), the keys that move them and change it,
// the pointer placing them, and it drawn scrolled to keep the caret in


// what's selected, from to
static void selection(const struct fui *ui, size_t *from, size_t *to) {
	*from = ui->caret < ui->anchor ? ui->caret : ui->anchor;
	*to = ui->caret < ui->anchor ? ui->anchor : ui->caret;
}

// the start of the character before at, and of the one after it
static size_t prev_char(const char *text, size_t at) {
	while (at > 0 && ((unsigned char)text[--at] & 0xc0) == 0x80) {
	}
	return at;
}

static size_t next_char(const char *text, size_t at) {
	if (text[at]) {
		at++;
	}
	while (((unsigned char)text[at] & 0xc0) == 0x80) {
		at++;
	}
	return at;
}

// what a character's part of, for words: letters and digits (any
// language's: anything past ascii), spaces, the rest
static int kind(unsigned char c) {
	if (c == '_' || c >= 0x80 || (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z')) {
		return 2;
	}
	return c == ' ' || c == '\t' ? 0 : 1;
}

// back over spaces and marks to the start of the word before at, and on to
// the end of the one after it
static size_t prev_word(const char *text, size_t at) {
	while (at > 0 && kind((unsigned char)text[at - 1]) != 2) {
		at--;
	}
	while (at > 0 && kind((unsigned char)text[at - 1]) == 2) {
		at--;
	}
	return at;
}

static size_t next_word(const char *text, size_t at) {
	while (text[at] && kind((unsigned char)text[at]) != 2) {
		at++;
	}
	while (text[at] && kind((unsigned char)text[at]) == 2) {
		at++;
	}
	return at;
}

// the caret and the selection kept on text: in it, on a character's start
// (the app may have changed it)
static void clamp(struct fui *ui, const char *text) {
	size_t len = strlen(text);
	size_t *ends[] = {&ui->caret, &ui->anchor};
	for (int i = 0; i < 2; i++) {
		size_t *at = ends[i];
		*at = *at > len ? len : *at;
		while (*at > 0 && ((unsigned char)text[*at] & 0xc0) == 0x80) {
			(*at)--;
		}
	}
}

// the line's state for id: another's (or none yet), it starts over with
// the caret at the end
static void sync(struct fui *ui, uint64_t id, const char *text) {
	if (ui->edit != id) {
		fui_edit_start(ui, id, text);
	}
	clamp(ui, text);
}

void fui_edit_start(struct fui *ui, uint64_t id, const char *text) {
	ui->edit = id;
	ui->caret = ui->anchor = text ? strlen(text) : 0;
	ui->edit_scroll = 0;
	ui->edit_drag = false;
}

void fui_select(struct fui *ui, uint64_t id, size_t start, size_t end) {
	ui->edit = id;
	ui->anchor = start;
	ui->caret = end;
	ui->edit_scroll = 0;
	ui->edit_drag = false;
	fui_redraw(ui);
}

// what's selected gone, the caret where it was
static bool cut_selection(struct fui *ui, char *text) {
	size_t from, to;
	selection(ui, &from, &to);
	if (from == to) {
		return false;
	}
	memmove(text + from, text + to, strlen(text + to) + 1);
	ui->caret = ui->anchor = from;
	return true;
}

// s in at the caret over what's selected, its control characters left out
// (a paste's newlines), as much as fits. true when text changed
static bool insert(struct fui *ui, char *text, int size, const char *s) {
	bool changed = cut_selection(ui, text);
	size_t len = strlen(text);
	for (const char *c = s; *c; c++) {
		unsigned char b = (unsigned char)*c;
		if (b < 0x20 || b == 0x7f) {
			continue;
		}
		// a character whole or not at all
		size_t n = 1;
		while (((unsigned char)c[n] & 0xc0) == 0x80) {
			n++;
		}
		if (len + n >= (size_t)size) {
			break;
		}
		memmove(text + ui->caret + n, text + ui->caret, len - ui->caret + 1);
		memcpy(text + ui->caret, c, n);
		ui->caret += n;
		len += n;
		c += n - 1;
		changed = true;
	}
	ui->anchor = ui->caret;
	return changed;
}

bool fui_edit_paste(struct fui *ui, uint64_t id, char *text, int size) {
	char *pasted = fui_pasted_text(ui, id);
	if (!pasted) {
		return false;
	}
	sync(ui, id, text);
	bool changed = insert(ui, text, size, pasted);
	free(ui->pasted_text);
	ui->pasted_text = NULL;
	return changed;
}

bool fui_key_types(const struct fui_key *k) {
	unsigned char c = (unsigned char)k->text[0];
	return c >= 0x20 && c != 0x7f && !(k->mods & (FUI_CTRL | FUI_ALT | FUI_SUPER));
}

int fui_edit_do(struct fui *ui, uint64_t id, const struct fui_key *k, char *text, int size) {
	sync(ui, id, text);
	if (k->mods & (FUI_ALT | FUI_SUPER)) {
		return FUI_EDIT_NOT_MINE;
	}
	bool shift = k->mods & FUI_SHIFT, ctrl = k->mods & FUI_CTRL;
	size_t from, to;
	selection(ui, &from, &to);
	xkb_keysym_t sym = xkb_keysym_to_lower(k->sym);
	size_t at = ui->caret;
	switch (sym) {
	case XKB_KEY_Left:
	case XKB_KEY_KP_Left:
		// past what's selected, it goes to its start
		at = from != to && !shift ? from : ctrl ? prev_word(text, at) : prev_char(text, at);
		break;
	case XKB_KEY_Right:
	case XKB_KEY_KP_Right:
		at = from != to && !shift ? to : ctrl ? next_word(text, at) : next_char(text, at);
		break;
	case XKB_KEY_Home:
	case XKB_KEY_KP_Home:
		at = 0;
		break;
	case XKB_KEY_End:
	case XKB_KEY_KP_End:
		at = strlen(text);
		break;
	case XKB_KEY_BackSpace:
	case XKB_KEY_Delete:
	case XKB_KEY_KP_Delete:
		if (shift && sym != XKB_KEY_BackSpace) {
			return FUI_EDIT_NOT_MINE;
		}
		if (from == to) {
			bool back = sym == XKB_KEY_BackSpace;
			ui->anchor = back ? (ctrl ? prev_word(text, at) : prev_char(text, at))
				: (ctrl ? next_word(text, at) : next_char(text, at));
		}
		return cut_selection(ui, text) ? FUI_EDIT_CHANGED : FUI_EDIT_MOVED;
	default:
		if (ctrl && !shift && sym == XKB_KEY_a) {
			ui->anchor = 0;
			ui->caret = strlen(text);
			return FUI_EDIT_MOVED;
		} else if (ctrl && !shift && (sym == XKB_KEY_c || sym == XKB_KEY_x)) {
			// nothing selected: copying copies it all
			if (from != to) {
				char *part = strndup(text + from, to - from);
				if (part) {
					fui_copy_text(ui, part);
					free(part);
				}
			} else if (sym == XKB_KEY_c) {
				fui_copy_text(ui, text);
			}
			return sym == XKB_KEY_x && cut_selection(ui, text) ? FUI_EDIT_CHANGED : FUI_EDIT_MOVED;
		} else if (ctrl && !shift && sym == XKB_KEY_v) {
			fui_paste_text(ui, id); // it comes a frame or so later
			return FUI_EDIT_MOVED;
		} else if (fui_key_types(k)) {
			return insert(ui, text, size, k->text) ? FUI_EDIT_CHANGED : FUI_EDIT_MOVED;
		}
		return FUI_EDIT_NOT_MINE;
	}
	ui->caret = at;
	if (!shift) {
		ui->anchor = at;
	}
	return FUI_EDIT_MOVED;
}

bool fui_edit_key(struct fui *ui, uint64_t id, int i, char *text, int size) {
	if (i < 0 || i >= ui->keys_len) {
		return false;
	}
	return fui_edit_do(ui, id, &ui->keys[i], text, size) != FUI_EDIT_NOT_MINE;
}

// the byte in text under px (layout x, from its start)
static size_t index_at(PangoLayout *l, const char *text, double px) {
	int index, trailing;
	pango_layout_xy_to_index(l, (int)(px * PANGO_SCALE), 0, &index, &trailing);
	size_t at = (size_t)index;
	for (; trailing > 0 && text[at]; trailing--) {
		at = next_char(text, at);
	}
	return at;
}

static double x_of(PangoLayout *l, size_t at) {
	PangoRectangle pos;
	pango_layout_index_to_pos(l, (int)at, &pos);
	return (double)pos.x / PANGO_SCALE;
}

void fui_edit_draw(struct fui *ui, uint64_t id, double x, double y, double w, double h,
		const char *text, const char *font_name, uint32_t rgba, uint32_t accent, bool caret,
		bool active, bool clicked) {
	cairo_t *cr = ui->cr;
	PangoLayout *l = pango_cairo_create_layout(cr);
	pango_layout_set_font_description(l, fui_font(ui, font_name));
	pango_layout_set_text(l, text, -1);
	int tw, th;
	pango_layout_get_pixel_size(l, &tw, &th);
	double ty = y + round((h - th) / 2);
	if (active) {
		sync(ui, id, text);
	}
	bool ours = active && ui->edit == id;
	double scroll = ours ? ui->edit_scroll : 0;

	// the pointer, where it's drawn (in an app's transform too)
	if (ours && (clicked || ui->edit_drag)) {
		double px = ui->mx * ui->scale120 / 120.0, py = ui->my * ui->scale120 / 120.0;
		cairo_device_to_user(cr, &px, &py);
		size_t at = index_at(l, text, px - x + scroll);
		if (clicked) {
			ui->edit_drag = true;
			if (ui->click_count >= 3) {
				ui->anchor = 0;
				ui->caret = strlen(text);
			} else if (ui->click_count == 2) {
				// the word under it (or the spaces, the marks)
				size_t from = at, to = at;
				size_t on = text[at] ? at : prev_char(text, at);
				int k = kind((unsigned char)text[on]);
				if (*text) {
					from = on;
					to = next_char(text, on);
					while (from > 0 && kind((unsigned char)text[from - 1]) == k) {
						from--;
					}
					while (text[to] && kind((unsigned char)text[to]) == k) {
						to++;
					}
				}
				ui->anchor = from;
				ui->caret = to;
				ui->edit_drag = false;
			} else {
				ui->caret = at;
				if (!(fui_mods(ui) & FUI_SHIFT)) {
					ui->anchor = at; // shift: what's selected goes to there
				}
			}
		} else if (ui->button_down) {
			ui->caret = at;
		}
		if (!ui->button_down) {
			ui->edit_drag = false;
		}
		clamp(ui, text);
	}

	// scrolled just enough to keep the caret in, no further than its end
	double caret_x = ours ? x_of(l, ui->caret) : 0;
	if (ours) {
		double room = w - 2;
		if (caret_x - scroll > room) {
			scroll = caret_x - room;
		} else if (caret_x < scroll) {
			scroll = caret_x;
		}
		if (tw - scroll < room) {
			scroll = tw > room ? tw - room : 0;
		}
		ui->edit_scroll = scroll;
	}

	cairo_save(cr);
	cairo_rectangle(cr, x - 1, y, w + 2, h);
	cairo_clip(cr);
	size_t from, to;
	selection(ui, &from, &to);
	bool selected = ours && from != to;
	if (selected) {
		double sx = x_of(l, from), ex = x_of(l, to);
		// behind what's selected: the caret's colour, see through
		fui_fill_rounded(ui, x + sx - scroll, ty, ex - sx, th, 3, (accent & 0xffffff00) | 0x66);
	}
	fui_color(ui, rgba);
	cairo_move_to(cr, x - scroll, ty);
	pango_cairo_show_layout(cr, l);
	if (ours && !selected && caret) {
		fui_fill_rounded(ui, x + caret_x - scroll, ty + 1, 1.6, th - 2, 0.8, accent);
	}
	cairo_restore(cr);
	g_object_unref(l);
	if (ui->edit_drag) {
		fui_redraw(ui); // the selection follows the pointer
	}
}

bool fui_edit_line(struct fui *ui, uint64_t id, double x, double y, double w, double h,
		char *text, int size, const char *font_name, uint32_t rgba, uint32_t accent, bool caret) {
	bool changed = fui_edit_paste(ui, id, text, size);
	if (changed) {
		fui_redraw(ui); // the app hears of it next frame
	}
	// a press on it, where it's drawn
	bool clicked = false;
	if (ui->pressed && !ui->click_used) {
		double px = ui->mx * ui->scale120 / 120.0, py = ui->my * ui->scale120 / 120.0;
		cairo_device_to_user(ui->cr, &px, &py);
		if (px >= x && px < x + w && py >= y && py < y + h) {
			clicked = ui->click_used = true;
		}
	}
	fui_edit_draw(ui, id, x, y, w, h, text, font_name, rgba, accent, caret, true, clicked);
	return changed;
}
