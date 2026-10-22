#include "icons.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void rgba(cairo_t *cr, uint32_t c) {
	cairo_set_source_rgba(cr, (c >> 24 & 0xff) / 255.0, (c >> 16 & 0xff) / 255.0,
		(c >> 8 & 0xff) / 255.0, (c & 0xff) / 255.0);
}

static void stop(cairo_pattern_t *p, double at, uint32_t c) {
	cairo_pattern_add_color_stop_rgba(p, at, (c >> 24 & 0xff) / 255.0, (c >> 16 & 0xff) / 255.0,
		(c >> 8 & 0xff) / 255.0, (c & 0xff) / 255.0);
}

// a rounded rect path, corners r
static void rounded(cairo_t *cr, double x, double y, double w, double h, double r) {
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
	cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
}

static void line_style(cairo_t *cr, double width) {
	cairo_set_line_width(cr, width);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
}

void icon_place(struct fui *ui, enum place place, double x, double y, double s, uint32_t color) {
	cairo_t *cr = fui_cairo(ui);
	cairo_save(cr);
	cairo_new_path(cr); // nothing of what was drawn before joins on
	cairo_translate(cr, x, y);
	cairo_scale(cr, s / 16, s / 16);
	rgba(cr, color);
	line_style(cr, 1.4);
	switch (place) {
	case PLACE_HOME:
		cairo_move_to(cr, 2, 8);
		cairo_line_to(cr, 8, 2.5);
		cairo_line_to(cr, 14, 8);
		cairo_move_to(cr, 3.8, 6.8);
		cairo_line_to(cr, 3.8, 13.5);
		cairo_line_to(cr, 12.2, 13.5);
		cairo_line_to(cr, 12.2, 6.8);
		cairo_stroke(cr);
		rounded(cr, 6.6, 9.5, 2.8, 4, 0.6);
		cairo_fill(cr);
		break;
	case PLACE_DESKTOP:
		rounded(cr, 1.8, 2.8, 12.4, 8.4, 1.4);
		cairo_stroke(cr);
		cairo_move_to(cr, 5.5, 13.6);
		cairo_line_to(cr, 10.5, 13.6);
		cairo_move_to(cr, 8, 11.2);
		cairo_line_to(cr, 8, 13.6);
		cairo_stroke(cr);
		break;
	case PLACE_DOCUMENTS:
		cairo_move_to(cr, 9.5, 1.8);
		cairo_line_to(cr, 3.5, 1.8);
		cairo_line_to(cr, 3.5, 14.2);
		cairo_line_to(cr, 12.5, 14.2);
		cairo_line_to(cr, 12.5, 4.8);
		cairo_close_path(cr);
		cairo_move_to(cr, 9.5, 1.8);
		cairo_line_to(cr, 9.5, 4.8);
		cairo_line_to(cr, 12.5, 4.8);
		cairo_move_to(cr, 5.8, 8);
		cairo_line_to(cr, 10.2, 8);
		cairo_move_to(cr, 5.8, 10.8);
		cairo_line_to(cr, 10.2, 10.8);
		cairo_stroke(cr);
		break;
	case PLACE_DOWNLOADS:
		cairo_arc(cr, 8, 8, 6.3, 0, 2 * M_PI);
		cairo_stroke(cr);
		cairo_move_to(cr, 8, 4.5);
		cairo_line_to(cr, 8, 11);
		cairo_move_to(cr, 5.2, 8.4);
		cairo_line_to(cr, 8, 11.2);
		cairo_line_to(cr, 10.8, 8.4);
		cairo_stroke(cr);
		break;
	case PLACE_PICTURES:
		rounded(cr, 1.8, 3, 12.4, 10, 1.6);
		cairo_stroke(cr);
		cairo_move_to(cr, 2.5, 12);
		cairo_line_to(cr, 6.2, 8);
		cairo_line_to(cr, 8.8, 10.6);
		cairo_line_to(cr, 10.6, 9);
		cairo_line_to(cr, 13.5, 12);
		cairo_stroke(cr);
		cairo_arc(cr, 10.6, 6, 1.2, 0, 2 * M_PI);
		cairo_fill(cr);
		break;
	case PLACE_MUSIC:
		cairo_move_to(cr, 6, 12);
		cairo_line_to(cr, 6, 3.2);
		cairo_line_to(cr, 13, 2);
		cairo_line_to(cr, 13, 10.8);
		cairo_stroke(cr);
		cairo_arc(cr, 4.4, 12, 1.9, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_arc(cr, 11.4, 10.8, 1.9, 0, 2 * M_PI);
		cairo_fill(cr);
		break;
	case PLACE_VIDEOS:
		rounded(cr, 1.8, 3.5, 9, 9, 1.6);
		cairo_stroke(cr);
		cairo_move_to(cr, 10.8, 7);
		cairo_line_to(cr, 14.2, 4.8);
		cairo_line_to(cr, 14.2, 11.2);
		cairo_line_to(cr, 10.8, 9);
		cairo_stroke(cr);
		break;
	case PLACE_COMPUTER:
		rounded(cr, 1.8, 4, 12.4, 8, 1.8);
		cairo_stroke(cr);
		cairo_arc(cr, 11.2, 8, 0.9, 0, 2 * M_PI);
		cairo_fill(cr);
		cairo_move_to(cr, 4.2, 8);
		cairo_line_to(cr, 8, 8);
		cairo_stroke(cr);
		break;
	case PLACE_TRASH:
		cairo_move_to(cr, 2.5, 4);
		cairo_line_to(cr, 13.5, 4);
		cairo_move_to(cr, 6, 4);
		cairo_line_to(cr, 6.6, 2);
		cairo_line_to(cr, 9.4, 2);
		cairo_line_to(cr, 10, 4);
		cairo_move_to(cr, 3.8, 4);
		cairo_line_to(cr, 4.6, 14);
		cairo_line_to(cr, 11.4, 14);
		cairo_line_to(cr, 12.2, 4);
		cairo_move_to(cr, 6.7, 6.8);
		cairo_line_to(cr, 6.9, 11.4);
		cairo_move_to(cr, 9.3, 6.8);
		cairo_line_to(cr, 9.1, 11.4);
		cairo_stroke(cr);
		break;
	case PLACE_FOLDER:
	case PLACE_NONE:
		cairo_move_to(cr, 1.8, 3.2);
		cairo_line_to(cr, 6.2, 3.2);
		cairo_line_to(cr, 7.6, 4.8);
		cairo_line_to(cr, 14.2, 4.8);
		cairo_line_to(cr, 14.2, 13);
		cairo_line_to(cr, 1.8, 13);
		cairo_close_path(cr);
		cairo_stroke(cr);
		break;
	}
	cairo_restore(cr);
}

void icon_folder(struct fui *ui, double x, double y, double s, enum place place) {
	cairo_t *cr = fui_cairo(ui);
	// in a square box: the folder's a bit wider than tall
	double w = s * 0.9, h = s * 0.72, fx = x + (s - w) / 2, fy = y + (s - h) / 2 + s * 0.04;
	double r = s * 0.06, tab = w * 0.38, tab_h = s * 0.08;
	// the back, its tab on it
	cairo_new_path(cr);
	cairo_move_to(cr, fx + r, fy - tab_h);
	cairo_line_to(cr, fx + tab - r, fy - tab_h);
	cairo_curve_to(cr, fx + tab, fy - tab_h, fx + tab + r * 0.5, fy, fx + tab + r * 1.4, fy);
	cairo_line_to(cr, fx + w - r, fy);
	cairo_arc(cr, fx + w - r, fy + r, r, -M_PI / 2, 0);
	cairo_line_to(cr, fx + w, fy + h * 0.5);
	cairo_line_to(cr, fx, fy + h * 0.5);
	cairo_line_to(cr, fx, fy - tab_h + r);
	cairo_arc(cr, fx + r, fy - tab_h + r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
	rgba(cr, 0x3d8fe0ff);
	cairo_fill(cr);
	// the front, lit from above
	double front_y = fy + h * 0.14;
	rounded(cr, fx, front_y, w, h - h * 0.14, r);
	cairo_pattern_t *g = cairo_pattern_create_linear(0, front_y, 0, fy + h);
	stop(g, 0, 0x7cc3ffff);
	stop(g, 1, 0x4fa2f2ff);
	cairo_set_source(cr, g);
	cairo_fill(cr);
	cairo_pattern_destroy(g);
	// its top edge catching the light
	cairo_move_to(cr, fx + r, front_y + 0.75);
	cairo_line_to(cr, fx + w - r, front_y + 0.75);
	rgba(cr, 0xffffff70);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
	if (place > PLACE_HOME && place != PLACE_FOLDER && place != PLACE_COMPUTER
			&& place != PLACE_TRASH) {
		// pressed into the front, like an engraving
		double g_s = h * 0.42;
		icon_place(ui, place, fx + (w - g_s) / 2, front_y + (h * 0.86 - g_s) / 2 + 1, g_s,
			0x2f78c4c0);
	}
}

// a file kind's color
static uint32_t kind_color(enum file_kind kind) {
	switch (kind) {
	case KIND_IMAGE:
		return 0x34c759ff;
	case KIND_VIDEO:
		return 0xaf52deff;
	case KIND_AUDIO:
		return 0xff2d55ff;
	case KIND_ARCHIVE:
		return 0xb08a5aff;
	case KIND_CODE:
		return 0x0a84ffff;
	case KIND_TEXT:
		return 0x8e8e93ff;
	case KIND_PDF:
		return 0xff3b30ff;
	case KIND_DOCUMENT:
		return 0x2f7dd1ff;
	case KIND_PROGRAM:
		return 0x5e5ce6ff;
	case KIND_FOLDER:
	case KIND_OTHER:
		break;
	}
	return 0x8e8e93ff;
}

void icon_file(struct fui *ui, double x, double y, double s, enum file_kind kind,
		const char *name) {
	cairo_t *cr = fui_cairo(ui);
	double h = s * 0.92, w = h * 0.76, px = x + (s - w) / 2, py = y + (s - h) / 2;
	double fold = w * 0.3, r = s * 0.05;
	// the page, its corner folded down
	cairo_new_path(cr);
	cairo_move_to(cr, px + r, py);
	cairo_line_to(cr, px + w - fold, py);
	cairo_line_to(cr, px + w, py + fold);
	cairo_line_to(cr, px + w, py + h - r);
	cairo_arc(cr, px + w - r, py + h - r, r, 0, M_PI / 2);
	cairo_line_to(cr, px + r, py + h);
	cairo_arc(cr, px + r, py + h - r, r, M_PI / 2, M_PI);
	cairo_line_to(cr, px, py + r);
	cairo_arc(cr, px + r, py + r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
	cairo_path_t *page = cairo_copy_path(cr);
	rgba(cr, 0xf5f5f7ff);
	cairo_fill(cr);
	cairo_new_path(cr);
	cairo_move_to(cr, px + w - fold, py);
	cairo_line_to(cr, px + w - fold, py + fold - r);
	cairo_arc_negative(cr, px + w - fold + r, py + fold - r, r, M_PI, M_PI / 2);
	cairo_line_to(cr, px + w, py + fold);
	cairo_close_path(cr);
	rgba(cr, 0xd4d4d9ff);
	cairo_fill(cr);
	cairo_append_path(cr, page);
	rgba(cr, 0x00000026);
	cairo_set_line_width(cr, 0.8);
	cairo_stroke(cr);
	cairo_path_destroy(page);

	uint32_t color = kind_color(kind);
	// text and code: lines on it
	if (kind == KIND_TEXT || kind == KIND_CODE || kind == KIND_DOCUMENT || kind == KIND_OTHER) {
		double lx = px + w * 0.18, lw = w * 0.64;
		for (int i = 0; i < 5; i++) {
			double ly = py + h * 0.3 + i * h * 0.075;
			double len = i % 3 == 2 ? lw * 0.6 : lw;
			rgba(cr, 0x00000022);
			cairo_rectangle(cr, lx, ly, len, s * 0.018 + 0.4);
			cairo_fill(cr);
		}
	} else if (kind != KIND_PROGRAM) {
		// the kind's glyph, big in the middle
		enum place glyph = kind == KIND_IMAGE ? PLACE_PICTURES : kind == KIND_VIDEO ? PLACE_VIDEOS
			: kind == KIND_AUDIO ? PLACE_MUSIC : PLACE_DOCUMENTS;
		double gs = w * 0.5;
		icon_place(ui, glyph, px + (w - gs) / 2, py + h * 0.28, gs, (color & 0xffffff00) | 0xcc);
	}
	// the extension on a band of the kind's color
	const char *dot = strrchr(name, '.');
	char ext[8] = "";
	if (dot && dot != name) {
		size_t n = 0;
		for (const char *c = dot + 1; *c && n < 4; c++) {
			ext[n++] = (char)toupper((unsigned char)*c);
		}
		ext[n] = '\0';
	}
	if (ext[0] && s >= 32) {
		char font[48];
		snprintf(font, sizeof(font), "Inter Bold %.1f", s * 0.105);
		double tw = fui_text_width(ui, ext, font);
		double bw = tw + s * 0.12, bh = s * 0.17;
		double bx = px + (w - bw) / 2, by = py + h * 0.72;
		rounded(cr, bx, by, bw, bh, bh * 0.3);
		rgba(cr, color);
		cairo_fill(cr);
		fui_text(ui, px + w / 2, by + (bh - s * 0.14) / 2 - s * 0.005, ext, font, 0xffffffff,
			FUI_CENTER, 0);
	} else if (s < 32) {
		// small: a stripe of its color at the bottom
		rgba(cr, color);
		cairo_rectangle(cr, px + w * 0.2, py + h * 0.78, w * 0.6, h * 0.08);
		cairo_fill(cr);
	}
}

void icon_picture(struct fui *ui, double x, double y, double s, cairo_surface_t *thumb) {
	cairo_t *cr = fui_cairo(ui);
	double tw = cairo_image_surface_get_width(thumb), th = cairo_image_surface_get_height(thumb);
	// the picture itself in the box, its corners a little round, a shadow
	// under it and a hairline round it so a dark one still has an edge
	double scale = fmin(s / tw, s / th);
	double w = tw * scale, h = th * scale;
	double px = x + (s - w) / 2, py = y + (s - h) / 2;
	double r = fmax(2, fmin(s * 0.05, 6));
	if (s >= 40) {
		for (int i = 3; i > 0; i--) {
			rounded(cr, px - i * 0.5, py + i * 0.6, w + i, h + i * 0.5, r + i * 0.5);
			rgba(cr, 0x00000018);
			cairo_fill(cr);
		}
	}
	cairo_save(cr);
	rounded(cr, px, py, w, h, r);
	cairo_clip(cr);
	cairo_translate(cr, px, py);
	cairo_scale(cr, scale, scale);
	cairo_set_source_surface(cr, thumb, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_paint(cr);
	cairo_restore(cr);
	rounded(cr, px + 0.5, py + 0.5, w - 1, h - 1, r);
	rgba(cr, 0xffffff1f);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
}
