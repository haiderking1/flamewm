#ifndef FLAMEUI_INTERNAL_H
#define FLAMEUI_INTERNAL_H

#include <pango/pangocairo.h>
#include <time.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "flameui.h"
#include "image/image.h"

// flameui on the inside: window.c keeps the wayland side and the input
// that came in since the last frame, ui.c turns that into frame time state
// for the app and draws the controls

#define FUI_EASES 512
#define FUI_INPUTS 16
#define FUI_CURSOR_HIDDEN -2 // cursor_shape: no cursor at all
#define FUI_KEYS 32
#define FUI_TIMERS 32
#define FUI_FONTS 16

struct fui_ease_slot {
	uint64_t id;
	double value, target;
	double from, t; // a glide's: where it set off from, how far along (0 to 1)
	double seen; // the frame it was last asked for: long ago, its slot's free
	bool used;
};

// a key typed since the last frame
struct fui_key {
	xkb_keysym_t sym;
	char text[8]; // what it types, utf-8, empty for none
	unsigned mods; // enum fui_mod, held as it went down
};

struct fui_watch {
	int fd; // -1 for unused
	void (*f)(struct fui *ui, int fd, void *data);
	void *data;
};
#define FUI_WATCHES 8

struct fui_timer {
	double at; // fui_now seconds, 0 for unused
	void (*f)(struct fui *ui, void *data);
	void *data;
};

struct fui_buffer {
	struct wl_buffer *buffer;
	void *data;
	int width, height, stride;
	size_t size;
	bool busy;
	// the memory under it, kept from size to size: a window growing back to
	// a size it's been isn't new memory to fault in, a frame late
	struct wl_shm_pool *pool;
	size_t capacity;
};

struct fui {
	struct fui_config config;
	bool running;

	// wayland
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_subcompositor *subcompositor;
	struct wp_content_type_manager_v1 *content_type;
	struct zwp_linux_dmabuf_v1 *dmabuf; // gpu pictures (a video decoded there)
	// a frame showing nothing: one pixel of no colour, scaled to the window
	struct wp_single_pixel_buffer_manager_v1 *single_pixel;
	struct wl_buffer *nothing;
	bool show_nothing;
	struct wl_shm *shm;
	struct wl_seat *seat;
	struct wl_pointer *pointer;
	struct wl_keyboard *keyboard;
	struct xdg_wm_base *wm;
	struct wp_fractional_scale_manager_v1 *fractional_manager;
	struct wp_viewporter *viewporter;
	struct wp_cursor_shape_manager_v1 *cursor_manager;
	struct wp_cursor_shape_device_v1 *cursor_device;
	struct flame_eyecandy_manager_v1 *eyecandy;

	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *toplevel;
	// a panel's instead
	struct zwlr_layer_shell_v1 *layer_shell;
	// the session's lock (config.lock): the manager, the first screen it
	// goes on, the lock and the surface on that screen
	struct ext_session_lock_manager_v1 *lock_manager;
	struct wl_output *output;
	struct ext_session_lock_v1 *session_lock;
	struct ext_session_lock_surface_v1 *lock_surface;
	bool caps; // caps lock's on
	int status; // what fui_run returns once it's quit: 1 when it went wrong
	struct zwlr_layer_surface_v1 *layer_surface;
	struct fui_panel panel;
	// while it's popped up: a surface under it over all the rest, one
	// clear pixel the screen's size, that only takes clicks
	struct wl_surface *catcher;
	struct zwlr_layer_surface_v1 *catcher_layer;
	struct wp_viewport *catcher_viewport;
	bool on_catcher, caught;
	bool panel_hidden; // unmapped, fui_panel_show brings it back
	// where clicks land: input_len rects, -1 for all of it. sent with the
	// frame that changed it
	struct fui_rect input[FUI_INPUTS];
	int input_len;
	bool input_dirty;
	struct wp_fractional_scale_v1 *fractional;
	struct wp_viewport *viewport;
	struct flame_glass_v1 *glass;
	bool configured;
	bool fullscreen; // flame made it so
	int width, height; // layout units, as flame last configured it
	uint32_t scale120; // the screen's scale, 120ths
	struct fui_buffer buffers[2];
	struct wl_callback *frame_callback;
	bool dirty; // a frame's wanted
	int wake[2]; // a pipe: fui_wake from other threads

	// the pointer as of now, and what happened since the last frame
	double mx, my;
	bool pointer_in, button_down;
	bool pressed, released; // the left button went down / up
	bool right_pressed, right_used;
	int click_count; // this press's place in a quick run of them
	double last_press, last_press_x, last_press_y;
	double scroll;
	// what scrolls (a wheel, fingers), and a wheel spinning faster and faster
	uint32_t axis_source;
	double wheel_at, wheel_last, wheel_accel;
	uint32_t pointer_serial;
	int cursor_shape, cursor_shown; // wp_cursor_shape_device_v1 shapes

	// the keyboard
	struct xkb_context *xkb;
	struct xkb_keymap *keymap;
	struct xkb_state *xkb_state;
	struct fui_key keys[FUI_KEYS];
	int keys_len;
	int32_t repeat_rate, repeat_delay;
	struct fui_key repeat_key;
	uint32_t repeat_code;
	double repeat_next;

	// frame time
	cairo_t *cr;
	double now, last_frame;
	bool moving; // something eased and isn't there yet
	struct fui_ease_slot eases[FUI_EASES];
	uint64_t active; // the control the button went down on, 0 for none
	uint64_t focus; // the field being typed into, 0 for none
	char *focus_before; // what it said when it got focus
	uint64_t entered; // the field enter was pressed in this frame, 0 for none
	char typed[64]; // fui_typed's
	// the line of text being edited (edit.c): which, where the caret is and
	// where what's selected starts (bytes into it, the same for nothing
	// selected), how far it's scrolled to keep the caret in, and a press on
	// it held (selecting)
	uint64_t edit;
	size_t caret, anchor;
	double edit_scroll;
	bool edit_drag;
	bool click_used; // a control took this frame's press

	uint32_t eyecandy_version;
	double glass_opacity, glass_opacity_sent; // all the glass faded to this
	struct fui_pane panes[16];
	int panes_len;
	bool panes_dirty;

	struct fui_timer timers[FUI_TIMERS];
	struct fui_watch watches[FUI_WATCHES];
	struct fui_thumbs *thumbs; // thumbs.c, NULL till the first's asked for
	struct fui_icons *icons; // icons.c, NULL till the first's asked for

	// drag and drop (dnd.c): the drag over the window, a drop being read, a
	// drop read for the app this frame, a drag of ours
	struct wl_data_device_manager *data_manager;
	struct wl_data_device *data_device;
	uint32_t press_serial; // the left button's last press: a drag starts off it
	struct wl_data_offer *dnd_offer;
	uint32_t dnd_serial;
	double dnd_x, dnd_y;
	enum fui_dnd_action dnd_told;
	struct wl_data_offer *drop_offer;
	char *drop_buf;
	size_t drop_len;
	double drop_x, drop_y;
	enum fui_dnd_action drop_action;
	char **dropped;
	size_t dropped_len;
	bool dropped_ready;
	// what's dragged over, read while it's over
	int peek_fd;
	char *peek_buf;
	size_t peek_len;
	char **peeked;
	size_t peeked_len;
	bool peeked_ready;
	// the clipboard: what's on it (anyone's), ours if it's ours, a paste
	// being read, what a paste got for the app this frame
	struct wl_data_offer *clip_offer;
	struct wl_data_source *clip_source;
	char *clip_uris, *clip_gnome, *clip_plain;
	char **clip_paths;
	size_t clip_paths_len;
	bool clip_cut;
	int paste_fd;
	char *paste_buf;
	size_t paste_len;
	uint64_t paste_field; // a field's paste, 0: files
	bool paste_gnome;
	char **pasted;
	size_t pasted_len;
	bool pasted_ready, pasted_cut;
	char *pasted_text;
	bool pasted_text_seen;
	uint32_t input_serial; // the last key or button press: the clipboard's set off it
	struct wl_data_source *drag_source;
	struct wl_surface *drag_icon;
	struct fui_buffer drag_buffer;
	char *drag_uris, *drag_paths;

	// the menu open, 0 for none: where, and where it was drawn last frame
	uint64_t menu;
	double menu_x, menu_y;
	double menu_box[4];
	bool menu_press; // this frame's press landed in it

	struct {
		char name[64];
		PangoFontDescription *desc;
	} fonts[FUI_FONTS];
	int fonts_len;
};

double fui_now(void);
// frame time, ui.c: before the app's frame, and after it
void fui_frame_begin(struct fui *ui, cairo_t *cr);
void fui_frame_end(struct fui *ui);
void fui_fonts_free(struct fui *ui);
// a font by its pango description, kept
const PangoFontDescription *fui_font(struct fui *ui, const char *name);
// edit.c, under the fields: a line's editing starts (focus came to it),
// the caret at text's end
void fui_edit_start(struct fui *ui, uint64_t id, const char *text);
// a key typable: not a control character (tab, ctrl+f...)
bool fui_key_types(const struct fui_key *k);
// key k edited into line id's text: what it did
enum { FUI_EDIT_NOT_MINE, FUI_EDIT_MOVED, FUI_EDIT_CHANGED };
int fui_edit_do(struct fui *ui, uint64_t id, const struct fui_key *k, char *text, int size);
// the clipboard's text, there for line id now, in at the caret: true when
// it changed text
bool fui_edit_paste(struct fui *ui, uint64_t id, char *text, int size);
// line id drawn in x, y, w x h (text in rgba, upright in the middle), the
// caret (shown or not, a blink) and what's selected in accent while it's
// active, a press on it this frame (clicked) placing the caret
void fui_edit_draw(struct fui *ui, uint64_t id, double x, double y, double w, double h,
	const char *text, const char *font_name, uint32_t rgba, uint32_t accent, bool caret,
	bool active, bool clicked);
void fui_thumbs_free(struct fui *ui);
void fui_icons_free(struct fui *ui);
// window.c's shared memory buffers, the drag icon's too
bool fui_buffer_make(struct fui *ui, struct fui_buffer *b, int width, int height);
void fui_buffer_free(struct fui_buffer *b);
// dnd.c
void fui_dnd_init(struct fui *ui);
void fui_dnd_frame_end(struct fui *ui);
void fui_dnd_finish(struct fui *ui);

// video.c: a video's (or a song's cover) frame, fit in max_width x
// max_height. fui_media_name: a name like a video's or a song's
bool fui_media_name(const char *path);
bool fui_media_frame(const char *path, int max_width, int max_height, struct image *out);
// thumbcache.c: the freedesktop thumbnail cache. the size kept for a
// thumbnail want big (256 or 512), one of path's as it was at mtime
int fui_cache_size(int want);
bool fui_cache_read(const char *path, time_t mtime, int size, struct image *out);
void fui_cache_write(const char *path, time_t mtime, int size, const struct image *thumb);

#endif
