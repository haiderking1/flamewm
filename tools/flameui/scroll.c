#include <math.h>

#include "internal.h"

// a box that scrolls: the wheel over it moves the app's scroll, it eases
// there, what's drawn in it is cut to it, and a thin bar at its right
// shows where it is while it moves (or the pointer's at it), dragged it
// scrolls too

#define BAR_W 4 // the bar, wider under the pointer
#define BAR_W_OVER 7
#define BAR_INSET 3 // off the box's right edge, and its ends
#define BAR_MIN 28 // the bar's shortest
#define SHOWN_FOR 0.9 // seconds it stays after scrolling stops

// the slot kept for id, the one seen longest ago taken over when they're
// all used
static struct fui_scroll_slot *slot_for(struct fui *ui, uint64_t id) {
	struct fui_scroll_slot *oldest = &ui->scrolls[0];
	for (int i = 0; i < FUI_SCROLLS; i++) {
		struct fui_scroll_slot *s = &ui->scrolls[i];
		if (s->id == id) {
			return s;
		}
		if (s->seen < oldest->seen) {
			oldest = s;
		}
	}
	*oldest = (struct fui_scroll_slot){.id = id};
	return oldest;
}

// the bar's time's up: drawn again to fade it
static void fade_later(struct fui *ui, void *data) {
	struct fui_scroll_slot *s = data;
	s->timer = false;
	fui_redraw(ui);
}

// the bar's thumb as it's drawn for the slot: where, how tall. false when
// what's in it fits
static bool thumb(const struct fui_scroll_slot *s, double eased, double *ty, double *th) {
	double track = s->h - 2 * BAR_INSET;
	if (s->content <= s->h + 0.5 || track <= 0) {
		return false;
	}
	*th = fmax(BAR_MIN, track * s->h / s->content);
	double most = s->content - s->h;
	*ty = s->y + BAR_INSET + (track - *th) * fmin(fmax(eased / most, 0), 1);
	return true;
}

double fui_scroll_begin(struct fui *ui, uint64_t id, double x, double y, double w, double h,
		double *scroll) {
	struct fui_scroll_slot *s = slot_for(ui, id);
	s->seen = ui->now;
	s->x = x;
	s->y = y;
	s->w = w;
	s->h = h;
	s->scroll = scroll;
	double was = *scroll;
	double most = fmax(s->content - h, 0);

	// the bar dragged: held on its thumb (as it was last frame), it follows
	double bx = x + w - BAR_INSET - BAR_W_OVER, ty, th;
	bool has_bar = thumb(s, s->eased, &ty, &th);
	if (has_bar && ui->pressed && !ui->click_used && fui_hover(ui, bx - 2, ty, BAR_W_OVER + 4, th)) {
		ui->click_used = true;
		s->grabbed = true;
		s->grab = ui->my - ty;
	}
	if (s->grabbed && !ui->button_down) {
		s->grabbed = false;
	}
	if (s->grabbed && has_bar) {
		double track = h - 2 * BAR_INSET - th;
		double at = (ui->my - s->grab - y - BAR_INSET) / fmax(track, 1);
		*scroll = fmin(fmax(at, 0), 1) * most;
	} else if (fui_hover(ui, x, y, w, h) && !(fui_mods(ui) & FUI_CTRL)) {
		*scroll += fui_scrolled(ui); // ctrl and the wheel: an app's zoom
	}

	// eased no further than there was last frame (end has this one's)
	double target = fmin(fmax(*scroll, 0), most);
	s->eased = fui_ease(ui, id, target, 24);
	if (*scroll != was || fabs(s->eased - target) > 0.5) {
		s->moved_at = ui->now;
	}
	if (ui->scroll_depth < FUI_SCROLL_DEPTH) {
		ui->scroll_stack[ui->scroll_depth] = s;
	}
	ui->scroll_depth++;
	cairo_save(ui->cr);
	cairo_rectangle(ui->cr, x, y, w, h);
	cairo_clip(ui->cr);
	return s->eased;
}

void fui_scroll_end(struct fui *ui, double content_height) {
	if (ui->scroll_depth <= 0) {
		return;
	}
	cairo_restore(ui->cr);
	ui->scroll_depth--;
	if (ui->scroll_depth >= FUI_SCROLL_DEPTH) {
		return;
	}
	struct fui_scroll_slot *s = ui->scroll_stack[ui->scroll_depth];
	s->content = content_height;
	// scrolled no further than there is
	double most = fmax(content_height - s->h, 0);
	*s->scroll = fmin(fmax(*s->scroll, 0), most);

	double ty, th;
	bool has_bar = thumb(s, s->eased, &ty, &th);
	double bx = s->x + s->w - BAR_INSET - BAR_W_OVER;
	bool near = has_bar && fui_hover(ui, bx - 6, s->y, BAR_W_OVER + 6 + BAR_INSET, s->h);
	bool recent = ui->now - s->moved_at < SHOWN_FOR;
	double shown = fui_ease(ui, s->id ^ 0x5c7011ull, has_bar && (recent || near || s->grabbed)
		? 1 : 0, 10);
	if (recent && !s->timer) {
		s->timer = true;
		fui_after(ui, (int)((SHOWN_FOR - (ui->now - s->moved_at)) * 1000) + 20, fade_later, s);
	}
	if (has_bar && shown > 0.01) {
		double wide = fui_ease(ui, s->id ^ 0x3a1dull, near || s->grabbed ? 1 : 0, 18);
		double bw = BAR_W + (BAR_W_OVER - BAR_W) * wide;
		double x = s->x + s->w - BAR_INSET - bw;
		uint32_t alpha = (uint32_t)lround((0x55 + 0x30 * wide) * shown);
		fui_fill_rounded(ui, x, ty, bw, th, bw / 2, 0xffffff00 | alpha);
	}
}
