#include "notes.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "look.h"

#define PAD 14
#define ICON 36
#define CARD_R 20
#define GAP 10
#define CARDS_UP 4 // cards at once, the rest wait their turn
#define CARD_LINES 3
#define LIST_LINES 4
#define ACTION_H 28
#define HEAD_H 34
#define EMPTY_H 64

static void tick(struct fui *ui, void *data) {
	fui_redraw(ui);
}

static const char *title_of(const struct note *n) {
	return *n->summary ? n->summary : n->app;
}

static bool has_picture(const struct note *n) {
	return n->picture || *n->image;
}

// how long ago it came: "now", "5m ago", "2h ago", then the day
static void ago(time_t when, char *out, size_t size) {
	long s = (long)difftime(time(NULL), when);
	if (s < 60) {
		snprintf(out, size, "now");
	} else if (s < 3600) {
		snprintf(out, size, "%ldm ago", s / 60);
	} else if (s < 86400) {
		snprintf(out, size, "%ldh ago", s / 3600);
	} else {
		struct tm tm;
		localtime_r(&when, &tm);
		strftime(out, size, "%a %-d %b", &tm);
	}
}

// the text's width in a card w wide: the icon at its left, a picture at
// its right
static double text_width(const struct note *n, double w) {
	return w - PAD - ICON - 11 - PAD - (has_picture(n) ? ICON + 10 : 0);
}

static double title_height(struct fui *ui, const struct note *n, double w) {
	return fui_text_box_height(ui, text_width(n, w), 1, title_of(n), F_TITLE);
}

// how tall it is as a card (lines of its body at most), measured once
static double card_height(struct fui *ui, struct note *n, double w, int lines) {
	double *kept = &n->heights[lines == CARD_LINES ? 0 : 1];
	if (*kept > 0) {
		return *kept;
	}
	double body = *n->body
		? 2 + fui_text_box_height(ui, text_width(n, w), lines, n->body, F_BODY) : 0;
	double h = PAD + fmax(ICON, title_height(ui, n, w) + body) + PAD;
	*kept = n->actions_len ? h + ACTION_H + 6 : h;
	return *kept;
}

// a picture in an s box at x, y: whole in it, or covering it (cut round
// cornered, r)
static void picture_at(struct fui *ui, cairo_surface_t *picture, double x, double y, double s,
		double r, bool cover) {
	cairo_t *cr = fui_cairo(ui);
	int pw = cairo_image_surface_get_width(picture), ph = cairo_image_surface_get_height(picture);
	if (pw <= 0 || ph <= 0) {
		return;
	}
	double k = cover ? fmax(s / pw, s / ph) : fmin(s / pw, s / ph);
	cairo_save(cr);
	if (r > 0) {
		fui_rounded(ui, x, y, s, s, r);
		cairo_clip(cr);
	}
	cairo_translate(cr, x + (s - pw * k) / 2, y + (s - ph * k) / 2);
	cairo_scale(cr, k, k);
	cairo_set_source_surface(cr, picture, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_paint(cr);
	cairo_restore(cr);
}

// the app's icon, else its first letter on a colour of its own
static void app_icon(struct fui *ui, const struct note *n, double x, double y) {
	bool failed;
	cairo_surface_t *icon = fui_icon(ui, n->icon, ICON * 2, &failed);
	if (icon) {
		picture_at(ui, icon, x, y, ICON, 0, false);
		return;
	}
	if (!failed) {
		return; // on its way
	}
	static const uint32_t colors[] = {
		0x0a84ffff, 0x30d158ff, 0xff9f0aff, 0xff375fff, 0xbf5af2ff, 0x64d2ffff, 0xff453aff,
	};
	fui_fill_rounded(ui, x, y, ICON, ICON, 9,
		colors[fui_id(n->app, 0) % (sizeof(colors) / sizeof(colors[0]))]);
	char letter[8] = "?";
	if (*n->app) {
		// its first character, however many bytes
		size_t len = 1;
		while (n->app[len] && ((unsigned char)n->app[len] & 0xc0) == 0x80 && len < 4) {
			len++;
		}
		memcpy(letter, n->app, len);
		letter[len] = '\0';
		if (letter[0] >= 'a' && letter[0] <= 'z') {
			letter[0] = (char)(letter[0] - 'a' + 'A');
		}
	}
	fui_text(ui, x + ICON / 2.0, y + 8, letter, "Inter Semi-Bold 14", TEXT, FUI_CENTER, 0);
}

// the picture that came with it, at its right
static void side_picture(struct fui *ui, const struct note *n, double x, double y) {
	cairo_surface_t *picture = n->picture;
	if (!picture && *n->image) {
		picture = fui_icon(ui, n->image, ICON * 2, NULL);
	}
	if (picture) {
		picture_at(ui, picture, x, y, ICON, 8, true);
	}
}

// text with a soft dark edge, wrapped to w in no more than lines: how tall
static double shadowed_box(struct fui *ui, double x, double y, double w, int lines,
		const char *text, const char *font, uint32_t color) {
	fui_text_box(ui, x, y + 0.6, w, lines, text, font, 0x00000059, FUI_LEFT);
	return fui_text_box(ui, x, y, w, lines, text, font, color, FUI_LEFT);
}

// one drawn as a card at x, y, w x h (its glass is the caller's). live: it
// takes the pointer (a list's card scrolled out of sight doesn't). pointed
// at, its close button at its corner. a click on it: its default action,
// or a card goes to the list. true while it's pointed at
static bool card(struct fui *ui, struct notes *ns, struct note *n, double x, double y, double w,
		double h, int lines, bool is_card, bool live) {
	uint32_t id = n->id;
	bool over = live && !n->closed && fui_hover(ui, x - 8, y - 8, w + 8, h + 8);
	double lit = fui_ease(ui, fui_id(is_card ? "card lit" : "list lit", id), over ? 1 : 0, 18);

	app_icon(ui, n, x + PAD, y + PAD);
	if (has_picture(n)) {
		side_picture(ui, n, x + w - PAD - ICON, y + PAD);
	}
	double tx = x + PAD + ICON + 11, tw = text_width(n, w);
	char when[32];
	ago(n->when, when, sizeof(when));
	double ww = fui_text_width(ui, when, F_SUB);
	shadowed_text(ui, tx + tw, y + PAD, when, F_SUB, TEXT_DIM, FUI_RIGHT, 0);
	shadowed_text(ui, tx, y + PAD - 1, title_of(n), F_TITLE, TEXT, FUI_LEFT, tw - ww - 8);
	if (*n->body) {
		shadowed_box(ui, tx, y + PAD - 1 + title_height(ui, n, w) + 2, tw, lines, n->body,
			F_BODY, TEXT_DIM);
	}

	// its actions, side by side across it
	if (n->actions_len) {
		double ay = y + h - PAD - ACTION_H + 4, aw = (w - 2 * PAD - (n->actions_len - 1) * 8)
			/ n->actions_len;
		for (int i = 0; i < n->actions_len; i++) {
			double ax = x + PAD + i * (aw + 8);
			bool on = live && fui_hover(ui, ax, ay, aw, ACTION_H);
			double glow = fui_ease(ui, fui_id(n->actions[i].key, (uint64_t)id << 8 | (uint64_t)i
				| (is_card ? 1ull << 40 : 0)), on ? 1 : 0, 20);
			fui_fill_rounded(ui, ax, ay, aw, ACTION_H, ACTION_H / 2.0,
				0xffffff00 | (uint32_t)(0x1f + 0x19 * glow));
			fui_text(ui, ax + aw / 2, ay + 6, n->actions[i].label, F_SUB, TEXT, FUI_CENTER,
				aw - 16);
			if (on) {
				fui_cursor_pointer(ui);
			}
			if (live && fui_clicked(ui, ax, ay, aw, ACTION_H)) {
				notify_invoke(ns->notify, id, n->actions[i].key);
			}
		}
	}

	// its close button, half out of its corner, while pointed at
	if (lit > 0.01) {
		cairo_t *cr = fui_cairo(ui);
		double cx = x + 5, cy = y + 5, r = 10;
		bool on = live && fui_hover(ui, cx - r, cy - r, 2 * r, 2 * r);
		cairo_new_path(cr); // not joined to where the text left off
		cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
		fui_color(ui, (on ? 0x48484a00 : 0x2c2c2e00) | (uint32_t)(0xf0 * lit));
		cairo_fill_preserve(cr);
		fui_color(ui, 0xffffff00 | (uint32_t)(0x33 * lit));
		cairo_set_line_width(cr, 0.8);
		cairo_stroke(cr);
		double a = 3.2;
		cairo_move_to(cr, cx - a, cy - a);
		cairo_line_to(cr, cx + a, cy + a);
		cairo_move_to(cr, cx + a, cy - a);
		cairo_line_to(cr, cx - a, cy + a);
		fui_color(ui, 0xffffff00 | (uint32_t)(0xe6 * lit));
		cairo_set_line_width(cr, 1.6);
		cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
		cairo_stroke(cr);
		if (on) {
			fui_cursor_pointer(ui);
		}
		if (live && fui_clicked(ui, cx - r, cy - r, 2 * r, 2 * r)) {
			notify_dismiss(ns->notify, id);
		}
	}

	if (over && (n->clickable || is_card)) {
		fui_cursor_pointer(ui);
	}
	if (live && !n->closed && fui_clicked(ui, x, y, w, h)) {
		if (n->clickable) {
			notify_invoke(ns->notify, id, "default");
		} else if (is_card) {
			notify_card_done(ns->notify, id);
		}
	}
	return over;
}

static void busy(struct notes *ns, uint32_t id) {
	if (ns->busy_len < (int)(sizeof(ns->busy) / sizeof(ns->busy[0]))) {
		ns->busy[ns->busy_len++] = id;
	}
}

// its card's place in the stack, made at y the first time
static struct place *place_of(struct notes *ns, uint32_t id, double y) {
	int max = (int)(sizeof(ns->places) / sizeof(ns->places[0]));
	for (int i = 0; i < ns->places_len; i++) {
		if (ns->places[i].id == id) {
			return &ns->places[i];
		}
	}
	if (ns->places_len == max) {
		ns->places_len--; // never: there are no more notes than places
	}
	ns->places[ns->places_len] = (struct place){.id = id, .y = y};
	return &ns->places[ns->places_len++];
}

static void add_pane(struct fui_pane *panes, int *n, int max, struct fui_pane pane) {
	if (*n < max) {
		panes[(*n)++] = pane;
	}
}

// ---- cards ----

double notes_cards(struct fui *ui, struct notes *ns, double right, double top, bool hidden,
		struct fui_pane *panes, int *n_panes, int max_panes, struct fui_rect *rects, int *n_rects,
		int max_rects, double *want) {
	double now = fui_time(ui), x0 = right - NOTES_W, bottom = 0, next = 0;
	int len = notify_count(ns->notify);
	len = len < 64 ? len : 64;
	// where each goes in the stack, and where it is on its way there. one
	// going stays where it was, the rest close up over it
	double h[64], cy[64], y = top;
	bool queued[64];
	struct place *place[64];
	int up_count = 0;
	*want = 0;
	for (int i = 0; i < len; i++) {
		struct note *n = notify_at(ns->notify, i);
		h[i] = card_height(ui, n, NOTES_W, CARD_LINES);
		// one of the first few: a place in the stack
		queued[i] = n->card && !n->closed && up_count < CARDS_UP && !hidden;
		place[i] = place_of(ns, n->id, y);
		if (queued[i]) {
			place[i]->y = y;
			*want = y + h[i];
			y += h[i] + GAP;
			up_count++;
		}
		cy[i] = fui_glide(ui, fui_id("card y", n->id), place[i]->y, 0.3);
	}
	for (int i = 0; i < len; i++) {
		struct note *n = notify_at(ns->notify, i);
		// a new one comes in from off the screen's edge once the one under
		// it has made room, and once the surface has grown for it
		bool room = true;
		for (int j = i + 1; j < len && !place[i]->entered; j++) {
			if (queued[j]) {
				room = cy[j] >= place[j]->y - 12;
				break;
			}
		}
		bool up = queued[i] && place[i]->y + h[i] + 8 <= fui_height(ui)
			&& (place[i]->entered || room);
		double in = fui_glide_from(ui, fui_id("card in", n->id), 0, up ? 1 : 0, 0.45);
		place[i]->entered = in > 0.002;
		bool over = false;
		if (in > 0.002) {
			double x = x0 + (1 - in) * (NOTES_W + 30);
			add_pane(panes, n_panes, max_panes, glass(x, cy[i], NOTES_W, h[i], CARD_R));
			if (*n_rects < max_rects) {
				rects[(*n_rects)++] = (struct fui_rect){x - 8, cy[i] - 8, NOTES_W + 8, h[i] + 8};
			}
			over = card(ui, ns, n, x, cy[i], NOTES_W, h[i], CARD_LINES, true, in > 0.9);
			bottom = fmax(bottom, cy[i] + h[i]);
			if (n->closed) {
				busy(ns, n->id);
			}
		}
		// its time: from when it's all the way in, and while it's pointed
		// at or hidden it waits
		if (n->card && !n->closed && n->until > 0) {
			if (over || in < 0.98) {
				n->until = fmax(n->until, now + 2);
			}
			if (now >= n->until) {
				notify_card_done(ns->notify, n->id);
			} else {
				next = next ? fmin(next, n->until) : n->until;
			}
		}
	}
	if (next) {
		fui_after(ui, (int)((next - now) * 1000) + 10, tick, ns);
	}
	return bottom;
}

// ---- the list ----

static bool any_open(struct notes *ns) {
	for (int i = 0; i < notify_count(ns->notify); i++) {
		if (!notify_at(ns->notify, i)->closed) {
			return true;
		}
	}
	return false;
}

// nothing in it, and nothing on its way out (this frame's)
static bool list_empty(struct notes *ns) {
	for (int i = 0; i < notify_count(ns->notify) && i < 64; i++) {
		if (ns->ins[i] > 0.002) {
			return false;
		}
	}
	return true;
}

double notes_list_height(struct fui *ui, struct notes *ns, double max) {
	// each one's way in or out once this frame, kept for the drawing
	double content = 8;
	int len = notify_count(ns->notify);
	for (int i = 0; i < len && i < 64; i++) {
		struct note *n = notify_at(ns->notify, i);
		ns->heights[i] = card_height(ui, n, NOTES_W, LIST_LINES);
		// one going slides out with its room kept, then the rest close up
		// over it (one coming: room made, then it slides in)
		double gone = 1 - fui_ramp(ui, fui_id("list in", n->id), n->closed ? 0 : 1, 0.55);
		ns->slides[i] = fui_soft(gone / 0.55);
		ns->ins[i] = 1 - fui_soft((gone - 0.45) / 0.55);
		content += (ns->heights[i] + GAP) * ns->ins[i];
	}
	ns->list_h = fmin(HEAD_H + (list_empty(ns) ? EMPTY_H + 16 : content), max);
	return ns->list_h;
}

void notes_list(struct fui *ui, struct notes *ns, double right, double top, double max,
		struct fui_pane *panes, int *n_panes, int max_panes) {
	double x0 = right - NOTES_W;
	shadowed_text(ui, x0 + 6, top + 6, "Notifications", F_BIG_TITLE, TEXT, FUI_LEFT, 0);
	if (any_open(ns)) {
		const char *label = "Clear All";
		double cw = fui_text_width(ui, label, F_SUB) + 24, cx = right - cw, cy = top + 4;
		bool on = fui_hover(ui, cx, cy, cw, 24);
		double glow = fui_ease(ui, fui_id("clear all", 0), on ? 1 : 0, 20);
		fui_fill_rounded(ui, cx, cy, cw, 24, 12, 0xffffff00 | (uint32_t)(0x1f + 0x19 * glow));
		fui_text(ui, cx + cw / 2, cy + 4, label, F_SUB, TEXT, FUI_CENTER, 0);
		if (on) {
			fui_cursor_pointer(ui);
		}
		if (fui_clicked(ui, cx, cy, cw, 24)) {
			notify_clear(ns->notify);
		}
	}
	double y0 = top + HEAD_H, view = ns->list_h - HEAD_H;
	if (list_empty(ns)) {
		add_pane(panes, n_panes, max_panes, glass(x0, y0 + 8, NOTES_W, EMPTY_H, CARD_R));
		fui_text(ui, x0 + NOTES_W / 2.0, y0 + 8 + EMPTY_H / 2.0 - 8, "No Notifications",
			F_TITLE, TEXT_DIM, FUI_CENTER, 0);
		return;
	}
	// scrolled with the wheel while it's pointed at, as far as there is
	double content = 8;
	for (int i = 0; i < notify_count(ns->notify) && i < 64; i++) {
		content += (ns->heights[i] + GAP) * ns->ins[i];
	}
	bool over = fui_hover(ui, x0 - 10, y0, NOTES_W + 20, view);
	double y = y0 + 8 - fui_scroll_begin(ui, fui_id("notes scroll", 0), x0 - 12, y0, NOTES_W + 24,
		view, &ns->scroll);
	for (int i = 0; i < notify_count(ns->notify) && i < 64; i++) {
		struct note *n = notify_at(ns->notify, i);
		double h = ns->heights[i], in = ns->ins[i];
		if (in > 0.002 && ns->slides[i] < 0.998 && y + h > y0 && y < y0 + view) {
			double x = x0 + ns->slides[i] * (NOTES_W + 30);
			// its glass cut to what shows
			double gy = fmax(y, y0), gh = fmin(y + h, y0 + view) - gy;
			add_pane(panes, n_panes, max_panes, glass(x, gy, NOTES_W, gh, CARD_R));
			card(ui, ns, n, x, y, NOTES_W, h, LIST_LINES, false, over && ns->slides[i] < 0.1);
		}
		if (n->closed && in > 0.002) {
			busy(ns, n->id);
		}
		y += (h + GAP) * in;
	}
	fui_scroll_end(ui, content);
}

void notes_done(struct notes *ns) {
	// closed, and nothing's showing it any more: let go
	for (int i = notify_count(ns->notify) - 1; i >= 0; i--) {
		struct note *n = notify_at(ns->notify, i);
		bool showing = false;
		for (int b = 0; b < ns->busy_len && !showing; b++) {
			showing = ns->busy[b] == n->id;
		}
		if (n->closed && !showing) {
			notify_forget(ns->notify, n->id);
		}
	}
	ns->busy_len = 0;
	// the places of the ones gone
	for (int i = 0; i < ns->places_len;) {
		bool there = false;
		for (int k = 0; k < notify_count(ns->notify) && !there; k++) {
			there = notify_at(ns->notify, k)->id == ns->places[i].id;
		}
		if (there) {
			i++;
		} else {
			ns->places[i] = ns->places[--ns->places_len];
		}
	}
}
