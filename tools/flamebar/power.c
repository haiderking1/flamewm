#include "power.h"

#include <math.h>
#include <stdio.h>

#include "look.h"

#define F_BAR "Inter Medium 10.5"
#define LOW 0xff453aff // the red it goes when it's nearly out

// a bolt: charging
static void bolt(cairo_t *cr, double cx, double cy, double s) {
	cairo_new_path(cr);
	cairo_move_to(cr, cx + s * 0.12, cy - s * 0.5);
	cairo_line_to(cr, cx - s * 0.3, cy + s * 0.08);
	cairo_line_to(cr, cx - s * 0.02, cy + s * 0.08);
	cairo_line_to(cr, cx - s * 0.12, cy + s * 0.5);
	cairo_line_to(cr, cx + s * 0.3, cy - s * 0.08);
	cairo_line_to(cr, cx + s * 0.02, cy - s * 0.08);
	cairo_close_path(cr);
}

double power_icon(struct fui *ui, const struct battery_now *now, double right, double cy) {
	cairo_t *cr = fui_cairo(ui);
	const double bw = 24, bh = 12, nub = 2;
	double bx = right - bw - nub, by = cy - bh / 2;
	bool low = !now->plugged && now->percent <= 10;
	// the outline and its nub, with the bar's soft shadow under them
	for (int shadow = 1; shadow >= 0; shadow--) {
		cairo_save(cr);
		cairo_translate(cr, 0, shadow * 0.6);
		fui_color(ui, shadow ? 0x00000040 : 0xffffffd9);
		fui_rounded(ui, bx + 0.5, by + 0.5, bw - 1, bh - 1, 3.5);
		cairo_set_line_width(cr, 1.1);
		cairo_stroke(cr);
		fui_fill_rounded(ui, bx + bw + 0.5, cy - 2, nub, 4, 1, shadow ? 0x00000040 : 0xffffff99);
		cairo_restore(cr);
	}
	// how full
	double fill = (bw - 4) * fmax(0, fmin(now->percent / 100, 1));
	if (fill > 0.5) {
		fui_fill_rounded(ui, bx + 2, by + 2, fmax(fill, 2), bh - 4, 2, low ? LOW : TEXT);
	}
	if (now->charging) {
		// the bolt cut out of the fill, outlined dark to read on white
		bolt(cr, bx + bw / 2, cy, bh + 2);
		fui_color(ui, 0x000000a6);
		cairo_set_line_width(cr, 1.6);
		cairo_stroke_preserve(cr);
		fui_color(ui, TEXT);
		cairo_fill(cr);
	}
	// how full in numbers, left of it
	char text[16];
	snprintf(text, sizeof(text), "%.0f%%", now->percent);
	double tw = shadowed_text(ui, bx - 6, cy - 8, text, F_BAR, TEXT, FUI_RIGHT, 0);
	return bw + nub + 6 + tw;
}

// "2 hr 34 min left", "45 min left", "1 hr until full", or what it's doing
static void how_long(const struct battery_now *now, char *out, size_t size) {
	int minutes = (int)(now->seconds / 60), hours = minutes / 60;
	minutes %= 60;
	char span[32];
	if (hours && minutes) {
		snprintf(span, sizeof(span), "%d hr %d min", hours, minutes);
	} else if (hours) {
		snprintf(span, sizeof(span), "%d hr", hours);
	} else {
		snprintf(span, sizeof(span), "%d min", minutes);
	}
	if (now->full) {
		snprintf(out, size, "Fully Charged");
	} else if (now->plugged && !now->charging) {
		snprintf(out, size, "Not Charging");
	} else if (!hours && !minutes) {
		snprintf(out, size, now->charging ? "Charging" : "Calculating\u2026");
	} else {
		snprintf(out, size, "%s %s", span, now->charging ? "until full" : "left");
	}
}

void power_card(struct fui *ui, const struct battery_now *now, double x, double y,
		struct fui_pane *panes, int *n, int max) {
	if (*n < max) {
		panes[(*n)++] = glass(x, y, POWER_CARD_W, POWER_CARD_H, 22);
	}
	char percent[16], left[64];
	snprintf(percent, sizeof(percent), "%.0f%%", now->percent);
	how_long(now, left, sizeof(left));
	shadowed_text(ui, x + 18, y + 16, "Battery", F_TITLE, TEXT, FUI_LEFT, 0);
	shadowed_text(ui, x + POWER_CARD_W - 18, y + 13, percent, F_BIG_TITLE, TEXT, FUI_RIGHT, 0);
	// a bar as full as it is, red when it's nearly out
	double bx = x + 18, bw = POWER_CARD_W - 36, by = y + 44;
	bool low = !now->plugged && now->percent <= 10;
	fui_fill_rounded(ui, bx, by, bw, 6, 3, 0xffffff33);
	fui_fill_rounded(ui, bx, by, fmax(bw * now->percent / 100, 6), 6, 3, low ? LOW : TEXT);
	shadowed_text(ui, x + 18, y + 62, now->plugged ? "Power Source: Power Adapter"
		: "Power Source: Battery", F_SUB, TEXT_DIM, FUI_LEFT, 0);
	shadowed_text(ui, x + 18, y + 78, left, F_SUB, TEXT_DIM, FUI_LEFT, 0);
}
