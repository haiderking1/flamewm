#ifndef FLAMEUI_INTERNAL_H
#define FLAMEUI_INTERNAL_H

#include <pango/pangocairo.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "flameui.h"

// flameui on the inside: window.c keeps the wayland side and the input
// that came in since the last frame, ui.c turns that into frame time state
// for the app and draws the controls

#define FUI_EASES 512
#define FUI_KEYS 32
#define FUI_TIMERS 16
#define FUI_FONTS 16

struct fui_ease_slot {
	uint64_t id;
	double value, target;
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
};

struct fui {
	struct fui_config config;
	bool running;

	// wayland
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
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
	struct wp_fractional_scale_v1 *fractional;
	struct wp_viewport *viewport;
	struct flame_glass_v1 *glass;
	bool configured;
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
	char focus_before[256]; // what it said when it got focus
	char typed[64]; // fui_typed's
	bool click_used; // a control took this frame's press

	struct fui_pane panes[16];
	int panes_len;
	bool panes_dirty;

	struct fui_timer timers[FUI_TIMERS];
	struct fui_watch watches[FUI_WATCHES];
	struct fui_thumbs *thumbs; // thumbs.c, NULL till the first's asked for

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
void fui_thumbs_free(struct fui *ui);

#endif
