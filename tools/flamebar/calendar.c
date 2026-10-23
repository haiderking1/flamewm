// the locale's first day of the week (_NL_TIME_FIRST_WEEKDAY) is glibc's
#define _GNU_SOURCE

#include "calendar.h"

#include <langinfo.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "look.h"

#define ACCENT 0xff7a2aff // flame's: today
#define PAD 14
#define CELL_W 38
#define CELL_H 34
#define MONTH_Y 78 // the month's name and its arrows
#define DAYS_Y 114 // the days of the week
#define GRID_Y 134 // the days
#define ARROW 26
#define F_DATE "Inter Semi-Bold 17"
#define F_DAY "Inter Medium 10"
#define F_TODAY "Inter Bold 10"
#define WHEEL_MONTH 40 // a wheel's notch is 45

// ---- days ----

// noon that day (no daylight saving hour lost or gained moves it to
// another), fields filled in: its weekday, a day past the month's end
// into the next
static struct tm day_at(int year, int mon, int mday) {
	struct tm t = {.tm_year = year - 1900, .tm_mon = mon, .tm_mday = mday, .tm_hour = 12,
		.tm_isdst = -1};
	mktime(&t);
	return t;
}

static int days_in(int year, int mon) {
	return day_at(year, mon + 1, 0).tm_mday;
}

static struct tm today(void) {
	time_t now = time(NULL);
	struct tm t;
	localtime_r(&now, &t);
	return day_at(t.tm_year + 1900, t.tm_mon, t.tm_mday);
}

// days from today to it: 0 today, negative before
static long days_from_today(int year, int mon, int mday) {
	struct tm a = today(), b = day_at(year, mon, mday);
	return lround(difftime(mktime(&b), mktime(&a)) / 86400);
}

// the week's first day where the user is: 0 Sunday, 1 Monday...
static int first_weekday(void) {
#ifdef __GLIBC__
	// glibc's: the first day, counted from the day its weeks start
	// (19971130 a Sunday, 19971201 a Monday)
	union {
		const char *string;
		uintptr_t word;
	} base = {.string = nl_langinfo(_NL_TIME_WEEK_1STDAY)};
	int first = nl_langinfo(_NL_TIME_FIRST_WEEKDAY)[0];
	int from = (unsigned)base.word == 19971201 ? 1 : 0;
	return first >= 1 && first <= 7 ? (from + first - 1) % 7 : 1;
#else
	return 1;
#endif
}

void calendar_open(struct calendar *c) {
	struct tm t = today();
	c->year = t.tm_year + 1900;
	c->mon = t.tm_mon;
	c->mday = t.tm_mday;
	c->month = c->from = c->year * 12 + c->mon;
	c->dir = 0;
	c->wheel = 0;
}

static void go(struct calendar *c, int month) {
	if (month == c->month) {
		return;
	}
	c->from = c->month;
	c->dir = month > c->month ? 1 : -1;
	c->month = month;
	c->slides++;
}

static void pick(struct calendar *c, int year, int mon, int mday) {
	c->year = year;
	c->mon = mon;
	c->mday = mday;
	go(c, year * 12 + mon);
}

// ---- drawing ----

// "Today", "Tomorrow", "In 4 days", "3 days ago"
static void how_far(long days, char *out, size_t size) {
	if (days == 0) {
		snprintf(out, size, "Today");
	} else if (days == 1) {
		snprintf(out, size, "Tomorrow");
	} else if (days == -1) {
		snprintf(out, size, "Yesterday");
	} else if (days > 0) {
		snprintf(out, size, "In %ld days", days);
	} else {
		snprintf(out, size, "%ld days ago", -days);
	}
}

static void chevron(struct fui *ui, double cx, double cy, bool right, uint32_t color) {
	cairo_t *cr = fui_cairo(ui);
	double d = right ? 1 : -1;
	cairo_new_path(cr);
	cairo_move_to(cr, cx - 2 * d, cy - 4.5);
	cairo_line_to(cr, cx + 2.5 * d, cy);
	cairo_line_to(cr, cx - 2 * d, cy + 4.5);
	fui_color(ui, color);
	cairo_set_line_width(cr, 1.8);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	cairo_stroke(cr);
}

// a round button, lit when it's pointed at: true when it's clicked
static bool round_button(struct fui *ui, uint64_t id, double x, double y, double w, double h,
		bool live) {
	bool on = live && fui_hover(ui, x, y, w, h);
	double lit = fui_ease(ui, id, on ? 1 : 0, 20);
	fui_fill_rounded(ui, x, y, w, h, h / 2, 0xffffff00 | (uint32_t)(0x14 + 0x1f * lit));
	if (on) {
		fui_cursor_pointer(ui);
	}
	return live && fui_clicked(ui, x, y, w, h);
}

// one month's days at x, y: today lit, the picked one ringed, the days of
// the months either side dimmed. clicked (live), a day's picked
static void month_grid(struct fui *ui, struct calendar *c, int month, double x, double y,
		int first, bool live) {
	int year = month / 12, mon = month % 12;
	int lead = (day_at(year, mon, 1).tm_wday - first + 7) % 7;
	int len = days_in(year, mon);
	struct tm now = today();
	for (int cell = 0; cell < 42; cell++) {
		// the day in this cell, maybe one of the months either side
		struct tm d = day_at(year, mon, cell - lead + 1);
		int dy = d.tm_year + 1900, dm = d.tm_mon;
		bool here = cell >= lead && cell < lead + len;
		bool is_today = dy == now.tm_year + 1900 && dm == now.tm_mon && d.tm_mday == now.tm_mday;
		bool picked = dy == c->year && dm == c->mon && d.tm_mday == c->mday;
		double cx = x + (cell % 7) * CELL_W + CELL_W / 2.0;
		double cy = y + (cell / 7) * CELL_H + CELL_H / 2.0;
		double r = 14;
		bool on = live && fui_hover(ui, cx - CELL_W / 2.0, cy - CELL_H / 2.0, CELL_W, CELL_H);
		double lit = fui_ease(ui, fui_id("calendar day", (uint64_t)(dy * 400 + dm * 32
			+ d.tm_mday)), on ? 1 : 0, 20);
		cairo_t *cr = fui_cairo(ui);
		if (is_today) {
			cairo_new_path(cr);
			cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
			fui_color(ui, here ? ACCENT : 0xff7a2a66);
			cairo_fill(cr);
		} else if (picked && here) {
			cairo_new_path(cr);
			cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
			fui_color(ui, 0xffffff38);
			cairo_fill(cr);
		} else if (lit > 0.01) {
			cairo_new_path(cr);
			cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
			fui_color(ui, 0xffffff00 | (uint32_t)(0x1f * lit));
			cairo_fill(cr);
		}
		if (picked && is_today && here) {
			// today picked: a ring round its dot
			cairo_new_path(cr);
			cairo_arc(cr, cx, cy, r + 2.5, 0, 2 * M_PI);
			fui_color(ui, 0xffffff8c);
			cairo_set_line_width(cr, 1.2);
			cairo_stroke(cr);
		}
		char number[8];
		snprintf(number, sizeof(number), "%d", d.tm_mday);
		uint32_t color = is_today ? TEXT : here ? TEXT : 0xffffff59;
		if (is_today || !here) {
			fui_text(ui, cx, cy - 8, number, is_today ? F_TODAY : F_DAY, color, FUI_CENTER, 0);
		} else {
			shadowed_text(ui, cx, cy - 8, number, F_DAY, color, FUI_CENTER, 0);
		}
		if (on) {
			fui_cursor_pointer(ui);
		}
		if (on && fui_clicked(ui, cx - CELL_W / 2.0, cy - CELL_H / 2.0, CELL_W, CELL_H)) {
			pick(c, dy, dm, d.tm_mday);
		}
	}
}

void calendar_card(struct fui *ui, struct calendar *c, double x, double y,
		struct fui_pane *panes, int *n, int max, bool live) {
	if (*n < max) {
		panes[(*n)++] = glass(x, y, CALENDAR_W, CALENDAR_H, 22);
	}
	cairo_t *cr = fui_cairo(ui);
	double right = x + CALENDAR_W - PAD - 4, left = x + PAD + 4;

	// the day picked, big: its weekday, how far it is and its week, its
	// date the whole width
	struct tm day = day_at(c->year, c->mon, c->mday);
	char weekday[64], date[96], far[48], week[32], about[96];
	strftime(weekday, sizeof(weekday), "%A", &day);
	strftime(date, sizeof(date), "%-d %B %Y", &day);
	strftime(week, sizeof(week), "Week %V", &day);
	how_far(days_from_today(c->year, c->mon, c->mday), far, sizeof(far));
	snprintf(about, sizeof(about), "%s \u00b7 %s", far, week);
	shadowed_text(ui, left, y + 16, weekday, F_TITLE, ACCENT, FUI_LEFT, 0);
	shadowed_text(ui, right, y + 17, about, F_SUB, TEXT_DIM, FUI_RIGHT, 0);
	shadowed_text(ui, left, y + 33, date, F_DATE, TEXT, FUI_LEFT, right - left);
	fui_fill_rounded(ui, x + PAD, y + 66, CALENDAR_W - 2 * PAD, 1, 0, 0xffffff1a);

	// the month, back and on, and back to today's when it's another
	struct tm shown = day_at(c->month / 12, c->month % 12, 1);
	char title[64];
	strftime(title, sizeof(title), "%B %Y", &shown);
	shadowed_text(ui, left, y + MONTH_Y + 4, title, F_BIG_TITLE, TEXT, FUI_LEFT, 0);
	double ay = y + MONTH_Y, ax = x + CALENDAR_W - PAD - ARROW;
	if (round_button(ui, fui_id("calendar on", 0), ax, ay, ARROW, ARROW, live)) {
		go(c, c->month + 1);
	}
	chevron(ui, ax + ARROW / 2.0, ay + ARROW / 2.0, true, TEXT);
	ax -= ARROW + 6;
	if (round_button(ui, fui_id("calendar back", 0), ax, ay, ARROW, ARROW, live)) {
		go(c, c->month - 1);
	}
	chevron(ui, ax + ARROW / 2.0, ay + ARROW / 2.0, false, TEXT);
	struct tm now = today();
	int this_month = (now.tm_year + 1900) * 12 + now.tm_mon;
	bool away = c->month != this_month || days_from_today(c->year, c->mon, c->mday) != 0;
	double back = fui_ease(ui, fui_id("calendar today", 0), away ? 1 : 0, 16);
	if (back > 0.01) {
		double tw = fui_text_width(ui, "Today", F_SUB) + 20, tx = ax - 8 - tw;
		cairo_push_group(cr);
		if (round_button(ui, fui_id("calendar today button", 0), tx, ay, tw, ARROW,
				live && away)) {
			int was = c->month;
			calendar_open(c);
			c->month = was;
			go(c, this_month); // slid back to it from where it was
		}
		fui_text(ui, tx + tw / 2, ay + 5, "Today", F_SUB, TEXT, FUI_CENTER, 0);
		cairo_pop_group_to_source(cr);
		cairo_paint_with_alpha(cr, back);
	}

	// the days of the week, from the one weeks start on where the user is
	int first = first_weekday();
	double gx = x + (CALENDAR_W - 7 * CELL_W) / 2.0;
	for (int i = 0; i < 7; i++) {
		struct tm wd = day_at(2023, 0, 1 + (first + i) % 7); // 1 January 2023 a Sunday
		char name[16];
		strftime(name, sizeof(name), "%a", &wd);
		// its first letter, however many bytes
		size_t len = 1;
		while (name[len] && ((unsigned char)name[len] & 0xc0) == 0x80 && len < 4) {
			len++;
		}
		name[len] = '\0';
		fui_text(ui, gx + i * CELL_W + CELL_W / 2.0, y + DAYS_Y, name, F_LIST_TITLE, TEXT_DIM,
			FUI_CENTER, 0);
	}

	// the wheel over it: a month a notch
	double gy = y + GRID_Y, gh = 6 * CELL_H;
	if (live && fui_hover(ui, x, y + MONTH_Y, CALENDAR_W, CALENDAR_H - MONTH_Y)) {
		double turned = fui_scrolled(ui);
		if (turned != 0) {
			if ((turned > 0) != (c->wheel > 0)) {
				c->wheel = 0; // the other way: from nothing
			}
			c->wheel += turned;
			int months = (int)(c->wheel / WHEEL_MONTH);
			if (months) {
				c->wheel -= months * WHEEL_MONTH;
				go(c, c->month + months);
			}
		}
	}

	// the month, slid in from the side it's on from, the one it was going
	// out the other
	double p = c->dir ? fui_glide_from(ui, fui_id("calendar slide", c->slides), 1, 0, 0.32)
		: 0;
	cairo_save(cr);
	cairo_rectangle(cr, x + 1, gy - 2, CALENDAR_W - 2, gh + 4);
	cairo_clip(cr);
	double slide = CALENDAR_W;
	if (p > 0.001) {
		cairo_push_group(cr);
		month_grid(ui, c, c->from, gx - c->dir * (1 - p) * slide, gy, first, false);
		cairo_pop_group_to_source(cr);
		cairo_paint_with_alpha(cr, p);
	}
	cairo_push_group(cr);
	month_grid(ui, c, c->month, gx + c->dir * p * slide, gy, first, live && p < 0.05);
	cairo_pop_group_to_source(cr);
	cairo_paint_with_alpha(cr, 1 - p);
	cairo_restore(cr);
}
