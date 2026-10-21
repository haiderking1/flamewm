// X cursors showing over X windows: a window's own cursor from a bitmap
// shows where the pointer is, XFixes hides it and shows it again, the
// cursor font's shapes come out as theme cursors, a grab's cursor wins over
// the window's. screenshots (grim -c) say what flame drew
// usage: xcursor FLAMECTL
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <xcb/xcb.h>
#include <xcb/render.h>
#include <xcb/xfixes.h>

static xcb_connection_t *c;
static const char *ctl;

static void settle(void) {
	free(xcb_get_input_focus_reply(c, xcb_get_input_focus(c), NULL));
	nanosleep(&(struct timespec){0, 400000000}, NULL);
}

static int px, py; // where the pointer is, root coords

// a screenshot with the pointer in it: the color just inside the pointer's
// square, which flame draws at the pointer
static void shot(const char *name) {
	char cmd[128];
	snprintf(cmd, sizeof(cmd), "grim -c -t ppm '%s.ppm'", name);
	if (system(cmd) != 0) {
		printf("%s: no screenshot\n", name);
		return;
	}
	snprintf(cmd, sizeof(cmd), "%s.ppm", name);
	FILE *f = fopen(cmd, "rb");
	int w, h, max;
	if (!f || fscanf(f, "P6 %d %d %d", &w, &h, &max) != 3 || fgetc(f) == EOF
			|| fseek(f, ((long)(py + 5) * w + px + 5) * 3, SEEK_CUR) != 0) {
		printf("%s: unreadable\n", name);
		if (f) {
			fclose(f);
		}
		return;
	}
	int r = fgetc(f), g = fgetc(f), b = fgetc(f);
	fclose(f);
	printf("%s: %s (%d %d %d)\n", name, r > 200 && g < 60 && b < 60 ? "red"
		: r > 200 && g > 200 && b > 200 ? "white" : r < 60 && g > 200 && b < 60 ? "green"
		: "other", r, g, b);
}

// what XFixes says the pointer looks like
static void print_image(const char *what) {
	xcb_xfixes_get_cursor_image_and_name_reply_t *r = xcb_xfixes_get_cursor_image_and_name_reply(
		c, xcb_xfixes_get_cursor_image_and_name(c), NULL);
	if (!r) {
		printf("%s: no reply\n", what);
		return;
	}
	printf("%s: %dx%d name [%.*s]\n", what, r->width, r->height,
		xcb_xfixes_get_cursor_image_and_name_name_length(r),
		xcb_xfixes_get_cursor_image_and_name_name(r));
	free(r);
}

int main(int argc, char **argv) {
	if (argc < 2) {
		return 2;
	}
	ctl = argv[1];
	c = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(c)) {
		return 2;
	}
	xcb_screen_t *s = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
	free(xcb_xfixes_query_version_reply(c, xcb_xfixes_query_version(c, 5, 0), NULL));

	xcb_window_t w = xcb_generate_id(c);
	xcb_create_window(c, XCB_COPY_FROM_PARENT, w, s->root, 0, 0, 400, 300, 0,
		XCB_WINDOW_CLASS_INPUT_OUTPUT, s->root_visual, 0, NULL);
	xcb_map_window(c, w);
	settle();
	xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(c, xcb_get_geometry(c, w), NULL);
	xcb_gcontext_t gc = xcb_generate_id(c);
	uint32_t white = 0xffffff;
	xcb_create_gc(c, gc, w, XCB_GC_FOREGROUND, &white);
	xcb_rectangle_t all = {0, 0, g ? g->width : 400, g ? g->height : 300};
	xcb_poly_fill_rectangle(c, w, gc, 1, &all);
	free(g);

	// a 16x16 bitmap, all ones: a red square, hotspot in its corner
	xcb_pixmap_t bits = xcb_generate_id(c);
	xcb_create_pixmap(c, 1, bits, w, 16, 16);
	xcb_gcontext_t one = xcb_generate_id(c);
	uint32_t pixel = 1;
	xcb_create_gc(c, one, bits, XCB_GC_FOREGROUND, &pixel);
	xcb_rectangle_t square = {0, 0, 16, 16};
	xcb_poly_fill_rectangle(c, bits, one, 1, &square);
	xcb_cursor_t red = xcb_generate_id(c);
	xcb_create_cursor(c, red, bits, bits, 0xffff, 0, 0, 0, 0, 0, 0, 0);
	xcb_change_window_attributes(c, w, XCB_CW_CURSOR, &red);
	// freed right away, the window keeps showing it
	xcb_free_cursor(c, red);
	xcb_free_pixmap(c, bits);
	settle();

	xcb_translate_coordinates_reply_t *t = xcb_translate_coordinates_reply(c,
		xcb_translate_coordinates(c, w, s->root, 0, 0), NULL);
	px = (t ? t->dst_x : 0) + 100;
	py = (t ? t->dst_y : 0) + 100;
	free(t);
	char cmd[600];
	snprintf(cmd, sizeof(cmd), "%s pointer move %d %d > /dev/null", ctl, px, py);
	if (system(cmd) != 0) {
		return 1;
	}
	settle();
	print_image("window's");
	shot("shown");

	xcb_xfixes_hide_cursor(c, w);
	settle();
	shot("hidden");
	xcb_xfixes_show_cursor(c, w);
	settle();
	shot("again");

	// the cursor font's I-beam: a theme cursor, flame's to draw
	xcb_font_t font = xcb_generate_id(c);
	xcb_open_font(c, font, 6, "cursor");
	xcb_cursor_t text = xcb_generate_id(c);
	xcb_create_glyph_cursor(c, text, font, font, 152, 153, 0, 0, 0, 0xffff, 0xffff, 0xffff);
	xcb_change_window_attributes(c, w, XCB_CW_CURSOR, &text);
	settle();
	print_image("font's");

	// a grab with its own cursor shows it, wherever the pointer is
	xcb_cursor_t cross = xcb_generate_id(c);
	xcb_create_glyph_cursor(c, cross, font, font, 34, 35, 0, 0, 0, 0xffff, 0xffff, 0xffff);
	xcb_grab_pointer_reply_t *gr = xcb_grab_pointer_reply(c, xcb_grab_pointer(c, 0, w, 0,
		XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC, XCB_NONE, cross, XCB_CURRENT_TIME), NULL);
	free(gr);
	settle();
	print_image("grab's");
	xcb_ungrab_pointer(c, XCB_CURRENT_TIME);
	settle();
	print_image("after the grab");

	// Xcursor's way (gtk, qt): an ARGB picture, its pixmap freed before the
	// cursor's made from it
	xcb_render_pictformat_t argb = 0;
	xcb_render_query_pict_formats_reply_t *f = xcb_render_query_pict_formats_reply(c,
		xcb_render_query_pict_formats(c), NULL);
	xcb_render_pictforminfo_iterator_t it = xcb_render_query_pict_formats_formats_iterator(f);
	for (; f && it.rem; xcb_render_pictforminfo_next(&it)) {
		if (it.data->depth == 32 && it.data->direct.alpha_mask == 0xff
				&& it.data->direct.red_shift == 16) {
			argb = it.data->id;
		}
	}
	free(f);
	xcb_pixmap_t pixmap = xcb_generate_id(c);
	xcb_create_pixmap(c, 32, pixmap, w, 24, 20);
	xcb_render_picture_t picture = xcb_generate_id(c);
	xcb_render_create_picture(c, picture, pixmap, argb, 0, NULL);
	xcb_render_color_t green = {0, 0xffff, 0, 0xffff};
	xcb_rectangle_t whole = {0, 0, 24, 20};
	xcb_render_fill_rectangles(c, XCB_RENDER_PICT_OP_SRC, picture, green, 1, &whole);
	xcb_free_pixmap(c, pixmap);
	xcb_cursor_t themed = xcb_generate_id(c);
	xcb_render_create_cursor(c, themed, picture, 3, 4);
	xcb_render_free_picture(c, picture);
	xcb_change_window_attributes(c, w, XCB_CW_CURSOR, &themed);
	settle();
	print_image("argb");
	shot("argb shot");
	xcb_disconnect(c);
	return 0;
}
