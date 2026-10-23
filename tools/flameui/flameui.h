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
#include <stddef.h>
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
	int width, height; // what it opens at
	// how small the user can make it. 0: it can't be resized, it stays
	// width x height (flame floats it)
	int min_width, min_height;
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
// all the glass faded together, blur and all: 1 for all of it (the start).
// glass that comes and goes with what's over it fades with it, no pop
void fui_set_glass_opacity(struct fui *ui, double opacity);
bool fui_has_glass(const struct fui *ui);

// draw another frame soon (data changed outside input), and a timer: f
// called once, ms from now
void fui_redraw(struct fui *ui);
void fui_after(struct fui *ui, int ms, void (*f)(struct fui *ui, void *data), void *data);
// from any thread: draw another frame soon (work done elsewhere is ready)
void fui_wake(struct fui *ui);
// in frame: this one shows nothing (what's under the window shows, as it
// is: a video layer filling it goes to the screen without flame drawing it)
void fui_nothing(struct fui *ui);

// fullscreen asked of flame, and whether it is
void fui_set_fullscreen(struct fui *ui, bool fullscreen);
bool fui_fullscreen(const struct fui *ui);

// ---- a layer: pictures under the window ----

// a surface of its own under the window's, showing pictures the app hands
// it (a video's frames): flame scales them on the gpu, they change without
// the window being drawn again, the window's glass over them frosts them
struct fui_layer;
struct fui_picture;
struct fui_layer *fui_layer_create(struct fui *ui);
void fui_layer_destroy(struct fui_layer *layer);
// a picture of the layer's, width x height, XRGB (cairo's RGB24): its
// pixels anyone's to write, from any thread, while it isn't on screen
struct fui_picture *fui_picture_create(struct fui_layer *layer, int width, int height);
// a picture in gpu memory (a dmabuf, one plane: fd's the caller's still),
// NULL when flame can't take it. its pixels aren't for the cpu
struct fui_picture *fui_picture_create_dmabuf(struct fui_layer *layer, int width, int height,
	uint32_t fourcc, uint64_t modifier, int fd, uint32_t offset, uint32_t stride);
void fui_picture_destroy(struct fui_picture *picture);
void *fui_picture_pixels(struct fui_picture *picture, int *stride);
bool fui_picture_on_screen(const struct fui_picture *picture);
// the layer shows picture at x, y, w x h (window coords), scaled. on
// screen it's flame's till released hears it's done with it
void fui_layer_show(struct fui_layer *layer, struct fui_picture *picture, double x, double y,
	double w, double h);
// the same picture somewhere else (the window was resized)
void fui_layer_place(struct fui_layer *layer, double x, double y, double w, double h);
void fui_layer_on_release(struct fui_layer *layer,
	void (*released)(struct fui_picture *picture, void *data), void *data);
// f once at the layer's next refresh (its frame callback), on the window's thread
void fui_layer_frame(struct fui_layer *layer, void (*f)(void *data), void *data);

// ---- inside frame ----

cairo_t *fui_cairo(struct fui *ui);
// the window's size now, layout units: what flame made it
int fui_width(const struct fui *ui);
int fui_height(const struct fui *ui);
double fui_time(const struct fui *ui); // seconds, monotonic

// a value easing toward target, kept by id from frame to frame. speed:
// 1/seconds it takes to get most of the way. the first call jumps there
double fui_ease(struct fui *ui, uint64_t id, double target, double speed);

// the pointer: where (window coords), over a box, clicked in it this frame
bool fui_hover(struct fui *ui, double x, double y, double w, double h);
bool fui_clicked(struct fui *ui, double x, double y, double w, double h);
double fui_scrolled(struct fui *ui); // this frame's wheel, layout units down
void fui_cursor_pointer(struct fui *ui); // a hand over what's clickable
void fui_cursor_hide(struct fui *ui); // no cursor over the window this frame (a video playing)

// a press's place in a run of them, quick and on the same spot: 1 a single
// click, 2 a double click. good in the frame fui_clicked says it's there
int fui_click_count(const struct fui *ui);
// the right button went down over a box this frame
bool fui_right_clicked(struct fui *ui, double x, double y, double w, double h);
// where the pointer is (window coords), and whether the left button's held
void fui_pointer(const struct fui *ui, double *x, double *y);
bool fui_button_held(const struct fui *ui);

// the keyboard, for an app's own keys (shortcuts, arrows). nothing while a
// field's being typed into
enum fui_mod {
	FUI_SHIFT = 1 << 0,
	FUI_CTRL = 1 << 1,
	FUI_ALT = 1 << 2,
	FUI_SUPER = 1 << 3,
};
unsigned fui_mods(const struct fui *ui); // held now
// sym went down this frame (or repeated) with exactly mods held. letters
// any case: ctrl+shift+n is 'n' with FUI_CTRL | FUI_SHIFT
bool fui_key_pressed(const struct fui *ui, uint32_t sym, unsigned mods);
bool fui_typing(const struct fui *ui); // a field has the keys
// what was typed this frame (no ctrl, alt or super), "" while a field types
const char *fui_typed(struct fui *ui);
// field id has the keys from now on, as if clicked (text: what it says now,
// escape goes back to it)
void fui_focus(struct fui *ui, uint64_t id, const char *text);

// fd has something to read: f's called from the window's loop, till
// fui_unwatch. a few at most
bool fui_watch(struct fui *ui, int fd, void (*f)(struct fui *ui, int fd, void *data), void *data);
void fui_unwatch(struct fui *ui, int fd);

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
// text wrapped to width, no more than lines of it (the last cut short with
// an ellipsis), aligned in it. returns its height
double fui_text_box(struct fui *ui, double x, double y, double width, int lines,
	const char *text, const char *font, uint32_t rgba, enum fui_align align);
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
// a search field, w wide: what's typed is searched as it's typed, true
// when text changed this frame. grab: typing anywhere with no other field
// taking the keys types into it. escape clears it
bool fui_search(struct fui *ui, uint64_t id, double x, double y, double w, char *text,
	int size, bool grab);
// a picture file's thumbnail (jpeg, png, webp), width x height pixels cut to
// cover that. worker threads make it, the window redraws when it's there:
// NULL till then, or when it can't be read (failed). kept a while
cairo_surface_t *fui_thumb(struct fui *ui, const char *path, int width, int height,
	bool *failed);
// the same, the whole picture in its shape, its longest side size pixels
cairo_surface_t *fui_thumb_fit(struct fui *ui, const char *path, int size, bool *failed);
// the ones asked for so far and not started aren't wanted (another folder)
void fui_thumbs_forget(struct fui *ui);

// ---- drag and drop ----

enum fui_dnd_action {
	FUI_DND_NONE,
	FUI_DND_COPY,
	FUI_DND_MOVE,
};

// files dragged over the window: where the pointer is (window coords).
// false for no drag of files
bool fui_dnd_over(struct fui *ui, double *x, double *y);
// the files being dragged over (their paths), once they've been read: NULL
// till then. a drop's action can be picked by where they're from
char **fui_dnd_files(struct fui *ui, size_t *len);
// what a drop there would do, said every frame the drag's over
void fui_dnd_accept(struct fui *ui, enum fui_dnd_action action);
// files dropped this frame (their paths), what to do with them and where:
// NULL for none. good till the frame's over
char **fui_dnd_dropped(struct fui *ui, size_t *len, enum fui_dnd_action *action, double *x,
	double *y);
// paths dragged out of the window, off the press that's held: wherever
// they're dropped (another app, this one) gets them as file uris. the
// icon under the pointer icon_width x icon_height, drawn by draw_icon with
// fui_* as in a frame. false when it can't start
bool fui_drag_files(struct fui *ui, char *const *paths, size_t len, int icon_width,
	int icon_height, void (*draw_icon)(struct fui *ui, void *data), void *data);
// a drag of ours is on
bool fui_dragging(const struct fui *ui);

// ---- the clipboard ----

// paths on the clipboard, copied or cut, the way file managers put them
// there (and as text for everything else)
void fui_copy_files(struct fui *ui, char *const *paths, size_t len, bool cut);
void fui_copy_text(struct fui *ui, const char *text);
void fui_clipboard_clear(struct fui *ui);
// files are on it (anyone's)
bool fui_clipboard_has_files(const struct fui *ui);
// path is cut on it, by this app
bool fui_clipboard_cut(const struct fui *ui, const char *path);
// the clipboard's files asked for: fui_pasted_files has them on a later
// frame (copied, or cut: to be moved). false for none on it
bool fui_paste_files(struct fui *ui);
char **fui_pasted_files(struct fui *ui, size_t *len, bool *cut);
// fields paste text themselves (ctrl+v), these are theirs
void fui_paste_text(struct fui *ui, uint64_t field);
char *fui_pasted_text(struct fui *ui, uint64_t field);

// a menu over everything at a point, like a right click opens: open it, then
// every frame draw it last with fui_menu. a click outside closes it
struct fui_menu_item {
	const char *label; // NULL: a line between groups
	const char *shortcut; // shown at the right, NULL for none
	bool disabled;
	bool checked; // a tick before it (a menu with any has room for them)
	bool heading; // a group's name over it, not picked
};
void fui_menu_open(struct fui *ui, uint64_t id, double x, double y);
bool fui_menu_is_open(const struct fui *ui, uint64_t id);
void fui_menu_close(struct fui *ui);
// draws menu id if it's open: the item picked this frame, -1 for none
int fui_menu(struct fui *ui, uint64_t id, const struct fui_menu_item *items, int count);

// a plain button, true when clicked
bool fui_button(struct fui *ui, uint64_t id, double x, double y, const char *label);

#endif
