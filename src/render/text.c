#include "render/text.h"

#include <drm_fourcc.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>

// pango lays the line out and cairo draws it into memory, premultiplied
// ARGB, which is what the renderer takes. drawn at the output's scale and
// shown at its layout size, so it's as sharp as the screen

struct wlr_texture *render_text(struct wlr_renderer *renderer, const char *text,
		const char *font, const float color[4], int max_width, float scale, int *width,
		int *height) {
	if (!text || !*text || max_width <= 0 || scale <= 0) {
		return NULL;
	}
	// a scratch surface just to measure
	cairo_surface_t *scratch = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	cairo_t *cr = cairo_create(scratch);
	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *desc = pango_font_description_from_string(font);
	pango_layout_set_font_description(layout, desc);
	pango_font_description_free(desc);
	pango_layout_set_text(layout, text, -1);
	pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
	pango_layout_set_width(layout, (int)(max_width * scale) * PANGO_SCALE);
	pango_layout_set_single_paragraph_mode(layout, TRUE);
	// the font sized for the output's pixels
	pango_cairo_context_set_resolution(pango_layout_get_context(layout), 96.0 * scale);
	pango_layout_context_changed(layout);
	int pw, ph;
	pango_layout_get_pixel_size(layout, &pw, &ph);
	cairo_destroy(cr);
	cairo_surface_destroy(scratch);
	if (pw <= 0 || ph <= 0) {
		g_object_unref(layout);
		return NULL;
	}

	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
	cr = cairo_create(surface);
	cairo_set_source_rgba(cr, color[0], color[1], color[2], color[3]);
	pango_cairo_update_layout(cr, layout);
	pango_cairo_show_layout(cr, layout);
	cairo_surface_flush(surface);
	struct wlr_texture *texture = NULL;
	if (cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS) {
		texture = wlr_texture_from_pixels(renderer, DRM_FORMAT_ARGB8888,
			(uint32_t)cairo_image_surface_get_stride(surface), (uint32_t)pw, (uint32_t)ph,
			cairo_image_surface_get_data(surface));
	}
	cairo_destroy(cr);
	cairo_surface_destroy(surface);
	g_object_unref(layout);
	*width = (int)ceilf(pw / scale);
	*height = (int)ceilf(ph / scale);
	return texture;
}
