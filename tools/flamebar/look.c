#include "look.h"

struct fui_pane glass(double x, double y, double w, double h, double r) {
	return (struct fui_pane){(int)x, (int)y, (int)w, (int)h, r, 8, 0x00000026, 1.6, 0.15, 22};
}

double shadowed_text(struct fui *ui, double x, double y, const char *text, const char *font,
		uint32_t color, int align, double max_width) {
	cairo_t *cr = fui_cairo(ui);
	cairo_push_group(cr);
	fui_text(ui, x, y + 0.6, text, font, 0x00000059, align, max_width);
	cairo_pop_group_to_source(cr);
	cairo_paint(cr);
	fui_text(ui, x, y, text, font, color, align, max_width);
	return fui_text_width(ui, text, font);
}
