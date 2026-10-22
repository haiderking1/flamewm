#ifndef FLAMEUI_H
#define FLAMEUI_H

// flameui: what flame's own apps are drawn with. a wayland window on flame's
// glass (flame-eyecandy-v1), cairo and pango drawing into shared memory at
// the screen's scale, sharp at fractional ones. immediate mode: every frame
// the app says what's where (a toggle here, bound to this bool), flameui
// draws it, tells what got clicked, and eases every change. it redraws on
// input, and frame after frame only while something's still moving

#include <cairo.h>
#include <stdbool.h>
#include <stdint.h>

struct fui;

// a pane of glass behind the window, window coords, see flame-eyecandy-v1.
// tint 0xRRGGBBAA, straight alpha
struct fui_pane {
	int x, y, width, height;
	double radius, blur;
	uint32_t tint;
	double saturation, edge, refraction;
};

struct fui_config {
	const char *app_id, *title;
	int width, height; // fixed: flame floats it
	// the window's rounded corners: flame draws no border around it, the
	// app draws its own edge. 0 for a plain window with flame's border
	double corner_radius;
	// called every frame: draw with fui_* into the window, width x height
	// layout units, whatever's under the pointer is known by then
	void (*frame)(struct fui *ui, void *data);
	void *data;
};

// NULL without a wayland display. runs till the window's closed or fui_quit
struct fui *fui_create(const struct fui_config *config);
int fui_run(struct fui *ui);
void fui_quit(struct fui *ui);
void fui_destroy(struct fui *ui);

// the glass, replaced whole. without flame (no flame-eyecandy) the window
// just draws over nothing, see fui_has_glass
void fui_set_glass(struct fui *ui, const struct fui_pane *panes, int len);
bool fui_has_glass(const struct fui *ui);

// draw another frame soon (data changed outside input), and a timer: f
// called once, ms from now
void fui_redraw(struct fui *ui);
void fui_after(struct fui *ui, int ms, void (*f)(struct fui *ui, void *data), void *data);

// ---- inside frame ----

cairo_t *fui_cairo(struct fui *ui);
double fui_time(const struct fui *ui); // seconds, monotonic

// a value easing toward target, kept by id from frame to frame. speed:
// 1/seconds it takes to get most of the way. the first call jumps there
double fui_ease(struct fui *ui, uint64_t id, double target, double speed);

// the pointer: where (window coords), over a box, clicked in it this frame
bool fui_hover(struct fui *ui, double x, double y, double w, double h);
bool fui_clicked(struct fui *ui, double x, double y, double w, double h);
double fui_scrolled(struct fui *ui); // this frame's wheel, layout units down
void fui_cursor_pointer(struct fui *ui); // a hand over what's clickable

// ids for the things below: any unique number, fui_id hashes a string
uint64_t fui_id(const char *s, uint64_t salt);

// ---- drawing ----

enum fui_align { FUI_LEFT, FUI_CENTER, FUI_RIGHT };

// rgba as 0xRRGGBBAA
void fui_color(struct fui *ui, uint32_t rgba);
void fui_rounded(struct fui *ui, double x, double y, double w, double h, double r);
void fui_fill_rounded(struct fui *ui, double x, double y, double w, double h, double r,
	uint32_t rgba);
// a line of text, y its top. font: a pango description ("Inter Semi-Bold
// 11"). returns its width
double fui_text(struct fui *ui, double x, double y, const char *text, const char *font,
	uint32_t rgba, enum fui_align align, double max_width);
double fui_text_width(struct fui *ui, const char *text, const char *font);
// text wrapped to width, returns its height
double fui_paragraph(struct fui *ui, double x, double y, double width, const char *text,
	const char *font, uint32_t rgba);

// ---- controls: drawn, and true when the user changed the value ----

// a switch, 40 x 24 at x, y
bool fui_toggle(struct fui *ui, uint64_t id, double x, double y, bool *value, bool enabled);
// a slider w wide, value kept in min..max on steps of step. it moves with
// the pointer, true once it's let go
bool fui_slider(struct fui *ui, uint64_t id, double x, double y, double w, double *value,
	double min, double max, double step, bool enabled);
// how wide fui_segmented draws labels
double fui_segmented_width(struct fui *ui, const char *const *labels, int count);
// options side by side, one picked, right aligned at right
bool fui_segmented(struct fui *ui, uint64_t id, double right, double y, const char *const *labels,
	int count, int *selected, bool enabled);
// color dots, one picked (or none: -1), right aligned at right
bool fui_swatches(struct fui *ui, uint64_t id, double right, double y, const uint32_t *colors,
	int count, int *selected, bool enabled);
// a line of text the user types into, w wide. true when they press enter
// (or click away) with it changed
bool fui_field(struct fui *ui, uint64_t id, double x, double y, double w, char *text,
	int size, const char *placeholder, bool enabled);
// a plain button, true when clicked
bool fui_button(struct fui *ui, uint64_t id, double x, double y, const char *label);

#endif
