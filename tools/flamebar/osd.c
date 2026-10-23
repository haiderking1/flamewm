#include "osd.h"

#include <math.h>

#include "look.h"

#define SHOWN 1.6 // seconds after the last key

static void tick(struct fui *ui, void *data) {
	fui_redraw(ui);
}

void osd_show(struct osd *osd, enum osd_kind kind, double level, bool muted, double now) {
	osd->kind = kind;
	osd->level = level;
	osd->muted = muted;
	osd->until = now + SHOWN;
}

// the level changed by hand: to the sound server or the screen, and shown
static void set_level(struct osd *osd, struct control *c, double level) {
	osd->level = level;
	switch (osd->kind) {
	case OSD_VOLUME:
		sound_set_volume(c->sound, SOUND_OUTPUT, level);
		break;
	case OSD_MIC:
		sound_set_volume(c->sound, SOUND_INPUT, level);
		break;
	case OSD_BRIGHTNESS:
		backlight_set(c->backlight, level);
		break;
	}
}

double osd_frame(struct fui *ui, struct osd *osd, struct control *c, double right, double top,
		bool hidden, struct fui_pane *panes, int *n_panes, int max_panes, struct fui_rect *rects,
		int *n_rects, int max_rects) {
	double now = fui_time(ui);
	bool up = !hidden && (now < osd->until || osd->dragging);
	double in = fui_glide_from(ui, fui_id("osd", 0), 0, up ? 1 : 0, up ? 0.25 : 0.3);
	if (in <= 0.002) {
		osd->dragging = false;
		return 0;
	}
	if (up) {
		fui_after(ui, (int)((osd->until - now) * 1000) + 20, tick, osd);
	}
	cairo_t *cr = fui_cairo(ui);
	double x = right - OSD_W, y = top - (1 - in) * 10;
	if (*n_panes < max_panes) {
		panes[(*n_panes)++] = glass(x, y, OSD_W, OSD_H, OSD_H / 2.0);
	}
	if (*n_rects < max_rects) {
		rects[(*n_rects)++] = (struct fui_rect){x, y, OSD_W, OSD_H};
	}
	cairo_push_group(cr);
	// its icon: how loud (waves), muted (crossed), the mic, the sun
	double level = fui_glide(ui, fui_id("osd level", 0), osd->level, 0.15);
	double ix = x + 28, iy = y + OSD_H / 2.0;
	fui_color(ui, osd->muted ? TEXT_DIM : TEXT);
	if (osd->kind == OSD_VOLUME) {
		icon_speaker(cr, ix, iy, 24, osd->muted ? -1 : level <= 0.001 ? 0 : level < 0.34 ? 1
			: level < 0.67 ? 2 : 3);
	} else if (osd->kind == OSD_MIC) {
		icon_mic(cr, ix, iy, 24, osd->muted);
	} else {
		icon_sun(cr, ix, iy, 24);
	}
	// the level, dragged to change it
	double bx = x + 54, bw = OSD_W - 54 - 26, by = iy - 3;
	fui_fill_rounded(ui, bx, by, bw, 6, 3, 0xffffff33);
	if (level > 0.001) {
		fui_fill_rounded(ui, bx, by, fmax(bw * level, 6), 6, 3,
			osd->muted ? 0xffffff59 : TEXT);
	}
	bool over = fui_hover(ui, bx - 8, y, bw + 16, OSD_H);
	if (over || osd->dragging) {
		fui_cursor_pointer(ui);
		osd->until = fmax(osd->until, now + 1); // pointed at, it stays
	}
	if ((over && fui_clicked(ui, bx - 8, y, bw + 16, OSD_H)) || osd->dragging) {
		double mx, my;
		fui_pointer(ui, &mx, &my);
		set_level(osd, c, fmax(0, fmin((mx - bx) / bw, 1)));
		osd->dragging = fui_button_held(ui);
		fui_redraw(ui);
	}
	cairo_pop_group_to_source(cr);
	cairo_paint_with_alpha(cr, in);
	return (OSD_H + 10) * in;
}
