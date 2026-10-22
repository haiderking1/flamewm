#define _GNU_SOURCE // memfd_create
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "cursor-shape-v1-client-protocol.h"
#include "flame-eyecandy-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "internal.h"
#include "viewporter-client-protocol.h"
#include "xdg-shell-client-protocol.h"

// the window: a fixed size xdg toplevel (flame floats those), drawn into
// shared memory at the screen's scale (fractional, through the viewporter:
// buffer pixels are screen pixels, text stays sharp), its glass handed to
// flame with each new set. one poll loop: the display, timers, key repeat

double fui_now(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

// ---- buffers ----

static void buffer_release(void *data, struct wl_buffer *buffer) {
	struct fui_buffer *b = data;
	b->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {buffer_release};

void fui_buffer_free(struct fui_buffer *b) {
	if (b->buffer) {
		wl_buffer_destroy(b->buffer);
	}
	if (b->data) {
		munmap(b->data, b->size);
	}
	*b = (struct fui_buffer){0};
}

bool fui_buffer_make(struct fui *ui, struct fui_buffer *b, int width, int height) {
	fui_buffer_free(b);
	int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);
	size_t size = (size_t)stride * (size_t)height;
	int fd = memfd_create("flameui", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t)size) < 0) {
		if (fd >= 0) {
			close(fd);
		}
		return false;
	}
	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		close(fd);
		return false;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(ui->shm, fd, (int32_t)size);
	b->buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
		WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	wl_buffer_add_listener(b->buffer, &buffer_listener, b);
	b->data = data;
	b->width = width;
	b->height = height;
	b->stride = stride;
	b->size = size;
	return true;
}

// a free buffer the window's size at its scale now
static struct fui_buffer *buffer_get(struct fui *ui) {
	int width = (int)ceil(ui->width * ui->scale120 / 120.0);
	int height = (int)ceil(ui->height * ui->scale120 / 120.0);
	for (int i = 0; i < 2; i++) {
		struct fui_buffer *b = &ui->buffers[i];
		if (b->busy) {
			continue;
		}
		if (b->buffer && b->width == width && b->height == height) {
			return b;
		}
		return fui_buffer_make(ui, b, width, height) ? b : NULL;
	}
	return NULL;
}

// ---- drawing a frame ----

static void glass_send(struct fui *ui) {
	if (!ui->glass || !ui->panes_dirty) {
		return;
	}
	flame_glass_v1_clear(ui->glass);
	for (int i = 0; i < ui->panes_len; i++) {
		const struct fui_pane *p = &ui->panes[i];
		flame_glass_v1_add_pane(ui->glass, p->x, p->y, p->width, p->height,
			wl_fixed_from_double(p->radius), wl_fixed_from_double(p->blur), p->tint,
			wl_fixed_from_double(p->saturation), wl_fixed_from_double(p->edge),
			wl_fixed_from_double(p->refraction));
	}
	ui->panes_dirty = false;
}

static void frame_done(void *data, struct wl_callback *callback, uint32_t time);
static const struct wl_callback_listener frame_listener = {frame_done};

static void draw(struct fui *ui) {
	if (!ui->configured || ui->frame_callback) {
		return;
	}
	struct fui_buffer *b = buffer_get(ui);
	if (!b) {
		return; // both with flame still, the next release brings one
	}
	ui->dirty = false;
	cairo_surface_t *target = cairo_image_surface_create_for_data(b->data, CAIRO_FORMAT_ARGB32,
		b->width, b->height, b->stride);
	cairo_t *cr = cairo_create(target);
	cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	cairo_scale(cr, ui->scale120 / 120.0, ui->scale120 / 120.0);
	fui_frame_begin(ui, cr);
	ui->config.frame(ui, ui->config.data);
	fui_frame_end(ui);
	cairo_destroy(cr);
	cairo_surface_flush(target);
	cairo_surface_destroy(target);

	glass_send(ui);
	if (ui->viewport) {
		wp_viewport_set_destination(ui->viewport, ui->width, ui->height);
	} else {
		wl_surface_set_buffer_scale(ui->surface, (int32_t)(ui->scale120 / 120));
	}
	wl_surface_attach(ui->surface, b->buffer, 0, 0);
	wl_surface_damage_buffer(ui->surface, 0, 0, b->width, b->height);
	ui->frame_callback = wl_surface_frame(ui->surface);
	wl_callback_add_listener(ui->frame_callback, &frame_listener, ui);
	wl_surface_commit(ui->surface);
	b->busy = true;
	if (ui->cursor_device && ui->pointer_in && ui->cursor_shape != ui->cursor_shown) {
		wp_cursor_shape_device_v1_set_shape(ui->cursor_device, ui->pointer_serial,
			(uint32_t)ui->cursor_shape);
		ui->cursor_shown = ui->cursor_shape;
	}
	// still moving: the next frame's wanted too
	if (ui->moving) {
		ui->dirty = true;
	}
}

static void frame_done(void *data, struct wl_callback *callback, uint32_t time) {
	struct fui *ui = data;
	wl_callback_destroy(callback);
	ui->frame_callback = NULL;
	if (ui->dirty) {
		draw(ui);
	}
}

void fui_redraw(struct fui *ui) {
	ui->dirty = true;
	// asked mid-frame (a menu opening): the next frame's, not one inside this
	if (!ui->cr) {
		draw(ui);
	}
}

void fui_set_glass(struct fui *ui, const struct fui_pane *panes, int len) {
	len = len > 16 ? 16 : len;
	if (len == ui->panes_len && !memcmp(panes, ui->panes, sizeof(*panes) * (size_t)len)) {
		return;
	}
	memcpy(ui->panes, panes, sizeof(*panes) * (size_t)len);
	ui->panes_len = len;
	ui->panes_dirty = true;
}

bool fui_has_glass(const struct fui *ui) {
	return ui->glass != NULL;
}

void fui_after(struct fui *ui, int ms, void (*f)(struct fui *ui, void *data), void *data) {
	for (int i = 0; i < FUI_TIMERS; i++) {
		if (!ui->timers[i].at) {
			ui->timers[i] = (struct fui_timer){fui_now() + ms / 1000.0, f, data};
			return;
		}
	}
}

// ---- xdg ----

static void wm_ping(void *data, struct xdg_wm_base *wm, uint32_t serial) {
	xdg_wm_base_pong(wm, serial);
}

static const struct xdg_wm_base_listener wm_listener = {wm_ping};

static void xdg_surface_configure(void *data, struct xdg_surface *surface, uint32_t serial) {
	struct fui *ui = data;
	xdg_surface_ack_configure(surface, serial);
	ui->configured = true;
	fui_redraw(ui);
}

static const struct xdg_surface_listener xdg_surface_listener = {xdg_surface_configure};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t width,
		int32_t height, struct wl_array *states) {
	struct fui *ui = data;
	// 0: flame leaves it to us. a fixed window keeps its size
	if (!ui->config.min_width || width <= 0 || height <= 0) {
		return;
	}
	ui->width = width > ui->config.min_width ? width : ui->config.min_width;
	ui->height = height > ui->config.min_height ? height : ui->config.min_height;
	fui_redraw(ui);
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel) {
	fui_quit(data);
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
};

static void preferred_scale(void *data, struct wp_fractional_scale_v1 *fractional,
		uint32_t scale) {
	struct fui *ui = data;
	if (scale && scale != ui->scale120) {
		ui->scale120 = scale;
		fui_redraw(ui);
	}
}

static const struct wp_fractional_scale_v1_listener fractional_listener = {preferred_scale};

// ---- pointer ----

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
		struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
	struct fui *ui = data;
	ui->pointer_in = true;
	ui->pointer_serial = serial;
	ui->mx = wl_fixed_to_double(x);
	ui->my = wl_fixed_to_double(y);
	ui->cursor_shown = -1;
	fui_redraw(ui);
}

static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
		struct wl_surface *surface) {
	struct fui *ui = data;
	ui->pointer_in = false;
	ui->mx = ui->my = -1;
	fui_redraw(ui);
}

static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time,
		wl_fixed_t x, wl_fixed_t y) {
	struct fui *ui = data;
	ui->mx = wl_fixed_to_double(x);
	ui->my = wl_fixed_to_double(y);
	fui_redraw(ui);
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial,
		uint32_t time, uint32_t button, uint32_t state) {
	struct fui *ui = data;
	bool down = state == WL_POINTER_BUTTON_STATE_PRESSED;
	if (button == BTN_RIGHT && down) {
		ui->right_pressed = true;
		fui_redraw(ui);
		return;
	}
	if (button != BTN_LEFT) {
		return;
	}
	ui->button_down = down;
	if (down) {
		// quick, and where the last one was: the next click of a double click
		double now = fui_now();
		bool again = now - ui->last_press < 0.4 && fabs(ui->mx - ui->last_press_x) < 5
			&& fabs(ui->my - ui->last_press_y) < 5;
		ui->click_count = again ? ui->click_count + 1 : 1;
		ui->last_press = now;
		ui->last_press_x = ui->mx;
		ui->last_press_y = ui->my;
		ui->pressed = true;
		ui->press_serial = serial;
	} else {
		ui->released = true;
	}
	fui_redraw(ui);
}

// a wheel notch comes as 15: about three lines here, like elsewhere. notches
// coming quick one after another go further each (up to WHEEL_ACCEL_MAX
// times), a fast spin flies through a long folder. fingers on a touchpad
// move it exactly as far as they go
#define WHEEL_SPEED 3.0
#define WHEEL_ACCEL_MAX 2.5

static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time,
		uint32_t axis, wl_fixed_t value) {
	struct fui *ui = data;
	if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
		return;
	}
	double v = wl_fixed_to_double(value);
	if (ui->axis_source == WL_POINTER_AXIS_SOURCE_WHEEL
			|| ui->axis_source == WL_POINTER_AXIS_SOURCE_WHEEL_TILT) {
		double now = fui_now();
		bool spinning = now - ui->wheel_at < 0.08 && (v > 0) == (ui->wheel_last > 0);
		ui->wheel_accel = spinning ? fmin(ui->wheel_accel * 1.2, WHEEL_ACCEL_MAX) : 1;
		ui->wheel_at = now;
		ui->wheel_last = v;
		v *= WHEEL_SPEED * ui->wheel_accel;
	}
	ui->scroll += v;
	fui_redraw(ui);
}

static void pointer_frame(void *data, struct wl_pointer *pointer) {
}

static void pointer_axis_source(void *data, struct wl_pointer *pointer, uint32_t source) {
	struct fui *ui = data;
	ui->axis_source = source;
}

static void pointer_axis_stop(void *data, struct wl_pointer *pointer, uint32_t time,
		uint32_t axis) {
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer, uint32_t axis,
		int32_t discrete) {
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
	.frame = pointer_frame,
	.axis_source = pointer_axis_source,
	.axis_stop = pointer_axis_stop,
	.axis_discrete = pointer_axis_discrete,
};

// ---- keyboard ----

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format,
		int32_t fd, uint32_t size) {
	struct fui *ui = data;
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}
	char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED) {
		return;
	}
	struct xkb_keymap *keymap = xkb_keymap_new_from_string(ui->xkb, map,
		XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map, size);
	if (!keymap) {
		return;
	}
	if (ui->xkb_state) {
		xkb_state_unref(ui->xkb_state);
	}
	if (ui->keymap) {
		xkb_keymap_unref(ui->keymap);
	}
	ui->keymap = keymap;
	ui->xkb_state = xkb_state_new(keymap);
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
		struct wl_surface *surface, struct wl_array *keys) {
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial,
		struct wl_surface *surface) {
	struct fui *ui = data;
	ui->repeat_code = 0;
}

static void push_key(struct fui *ui, const struct fui_key *key) {
	if (ui->keys_len < FUI_KEYS) {
		ui->keys[ui->keys_len++] = *key;
	}
	fui_redraw(ui);
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial,
		uint32_t time, uint32_t key, uint32_t state) {
	struct fui *ui = data;
	if (!ui->xkb_state) {
		return;
	}
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
		if (ui->repeat_code == key) {
			ui->repeat_code = 0;
		}
		return;
	}
	struct fui_key k = {.sym = xkb_state_key_get_one_sym(ui->xkb_state, key + 8),
		.mods = fui_mods(ui)};
	xkb_state_key_get_utf8(ui->xkb_state, key + 8, k.text, sizeof(k.text));
	if ((unsigned char)k.text[0] < 0x20 || k.text[0] == 0x7f) {
		k.text[0] = '\0'; // control characters aren't text
	}
	push_key(ui, &k);
	if (ui->repeat_rate > 0 && xkb_keymap_key_repeats(ui->keymap, key + 8)) {
		ui->repeat_key = k;
		ui->repeat_code = key;
		ui->repeat_next = fui_now() + ui->repeat_delay / 1000.0;
	}
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial,
		uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
	struct fui *ui = data;
	if (ui->xkb_state) {
		xkb_state_update_mask(ui->xkb_state, depressed, latched, locked, 0, 0, group);
	}
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard, int32_t rate,
		int32_t delay) {
	struct fui *ui = data;
	ui->repeat_rate = rate;
	ui->repeat_delay = delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = keyboard_repeat_info,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
	struct fui *ui = data;
	if (caps & WL_SEAT_CAPABILITY_POINTER && !ui->pointer) {
		ui->pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(ui->pointer, &pointer_listener, ui);
		if (ui->cursor_manager) {
			ui->cursor_device = wp_cursor_shape_manager_v1_get_pointer(ui->cursor_manager,
				ui->pointer);
		}
	}
	if (caps & WL_SEAT_CAPABILITY_KEYBOARD && !ui->keyboard) {
		ui->keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(ui->keyboard, &keyboard_listener, ui);
	}
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {
}

static const struct wl_seat_listener seat_listener = {seat_capabilities, seat_name};

// ---- globals ----

static void global(void *data, struct wl_registry *registry, uint32_t name,
		const char *interface, uint32_t version) {
	struct fui *ui = data;
	if (!strcmp(interface, wl_compositor_interface.name)) {
		ui->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (!strcmp(interface, wl_shm_interface.name)) {
		ui->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (!strcmp(interface, wl_seat_interface.name) && !ui->seat) {
		ui->seat = wl_registry_bind(registry, name, &wl_seat_interface, version < 5 ? version : 5);
	} else if (!strcmp(interface, xdg_wm_base_interface.name)) {
		ui->wm = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
	} else if (!strcmp(interface, wp_fractional_scale_manager_v1_interface.name)) {
		ui->fractional_manager = wl_registry_bind(registry, name,
			&wp_fractional_scale_manager_v1_interface, 1);
	} else if (!strcmp(interface, wp_viewporter_interface.name)) {
		ui->viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
	} else if (!strcmp(interface, wp_cursor_shape_manager_v1_interface.name)) {
		ui->cursor_manager = wl_registry_bind(registry, name,
			&wp_cursor_shape_manager_v1_interface, 1);
	} else if (!strcmp(interface, wl_data_device_manager_interface.name)) {
		ui->data_manager = wl_registry_bind(registry, name, &wl_data_device_manager_interface,
			version < 3 ? version : 3);
	} else if (!strcmp(interface, flame_eyecandy_manager_v1_interface.name)) {
		ui->eyecandy = wl_registry_bind(registry, name, &flame_eyecandy_manager_v1_interface, 1);
	}
}

static void global_remove(void *data, struct wl_registry *registry, uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {global, global_remove};

// ---- the app ----

struct fui *fui_create(const struct fui_config *config) {
	struct fui *ui = calloc(1, sizeof(*ui));
	if (!ui) {
		return NULL;
	}
	ui->config = *config;
	ui->scale120 = 120;
	ui->mx = ui->my = -1;
	ui->cursor_shown = -1;
	ui->cursor_shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
	ui->repeat_rate = 25;
	ui->repeat_delay = 600;
	// other threads wake it down this
	ui->wake[0] = ui->wake[1] = -1;
	for (int i = 0; i < FUI_WATCHES; i++) {
		ui->watches[i].fd = -1;
	}
	if (pipe(ui->wake) == 0) {
		for (int i = 0; i < 2; i++) {
			fcntl(ui->wake[i], F_SETFD, FD_CLOEXEC);
			fcntl(ui->wake[i], F_SETFL, O_NONBLOCK);
		}
	}
	ui->display = wl_display_connect(NULL);
	ui->xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!ui->display || !ui->xkb) {
		fui_destroy(ui);
		return NULL;
	}
	ui->registry = wl_display_get_registry(ui->display);
	wl_registry_add_listener(ui->registry, &registry_listener, ui);
	wl_display_roundtrip(ui->display);
	if (!ui->compositor || !ui->shm || !ui->wm) {
		fprintf(stderr, "flameui: the compositor lacks what a window needs\n");
		fui_destroy(ui);
		return NULL;
	}
	xdg_wm_base_add_listener(ui->wm, &wm_listener, ui);
	if (ui->seat) {
		wl_seat_add_listener(ui->seat, &seat_listener, ui);
	}
	fui_dnd_init(ui);
	ui->surface = wl_compositor_create_surface(ui->compositor);
	if (ui->fractional_manager && ui->viewporter) {
		ui->fractional = wp_fractional_scale_manager_v1_get_fractional_scale(
			ui->fractional_manager, ui->surface);
		wp_fractional_scale_v1_add_listener(ui->fractional, &fractional_listener, ui);
		ui->viewport = wp_viewporter_get_viewport(ui->viewporter, ui->surface);
	}
	if (ui->eyecandy) {
		ui->glass = flame_eyecandy_manager_v1_get_glass(ui->eyecandy, ui->surface);
		if (config->corner_radius > 0) {
			flame_glass_v1_set_shape(ui->glass, wl_fixed_from_double(config->corner_radius));
		}
	}
	ui->xdg_surface = xdg_wm_base_get_xdg_surface(ui->wm, ui->surface);
	xdg_surface_add_listener(ui->xdg_surface, &xdg_surface_listener, ui);
	ui->toplevel = xdg_surface_get_toplevel(ui->xdg_surface);
	xdg_toplevel_add_listener(ui->toplevel, &toplevel_listener, ui);
	xdg_toplevel_set_title(ui->toplevel, config->title);
	xdg_toplevel_set_app_id(ui->toplevel, config->app_id);
	ui->width = config->width;
	ui->height = config->height;
	if (config->min_width) {
		xdg_toplevel_set_min_size(ui->toplevel, config->min_width, config->min_height);
	} else {
		xdg_toplevel_set_min_size(ui->toplevel, config->width, config->height);
		xdg_toplevel_set_max_size(ui->toplevel, config->width, config->height);
	}
	wl_surface_commit(ui->surface);
	ui->running = true;
	return ui;
}

void fui_quit(struct fui *ui) {
	ui->running = false;
}

// timers and key repeat due now, and when the next one is (ms, -1 for none)
static int run_due(struct fui *ui) {
	double now = fui_now();
	double next = 0;
	for (int i = 0; i < FUI_TIMERS; i++) {
		struct fui_timer *t = &ui->timers[i];
		if (t->at && t->at <= now) {
			struct fui_timer fired = *t;
			t->at = 0;
			fired.f(ui, fired.data);
		} else if (t->at && (!next || t->at < next)) {
			next = t->at;
		}
	}
	if (ui->repeat_code && ui->repeat_rate > 0) {
		if (ui->repeat_next <= now) {
			push_key(ui, &ui->repeat_key);
			ui->repeat_next = now + 1.0 / ui->repeat_rate;
		}
		next = !next || ui->repeat_next < next ? ui->repeat_next : next;
	}
	if (!next) {
		return -1;
	}
	int ms = (int)ceil((next - fui_now()) * 1000);
	return ms > 0 ? ms : 0;
}

int fui_run(struct fui *ui) {
	struct pollfd fds[2 + FUI_WATCHES] = {
		{.fd = wl_display_get_fd(ui->display), .events = POLLIN},
		{.fd = ui->wake[0], .events = POLLIN},
	};
	struct pollfd *fd = &fds[0];
	while (ui->running) {
		for (int i = 0; i < FUI_WATCHES; i++) {
			fds[2 + i] = (struct pollfd){.fd = ui->watches[i].fd, .events = POLLIN};
		}
		while (wl_display_prepare_read(ui->display) != 0) {
			wl_display_dispatch_pending(ui->display);
		}
		if (wl_display_flush(ui->display) < 0 && errno != EAGAIN) {
			wl_display_cancel_read(ui->display);
			return 1;
		}
		int timeout = run_due(ui);
		if (!ui->running) {
			wl_display_cancel_read(ui->display);
			break;
		}
		// a negative fd (no wake pipe, an unused watch) is skipped
		if (poll(fds, 2 + FUI_WATCHES, timeout) < 0 && errno != EINTR) {
			wl_display_cancel_read(ui->display);
			return 1;
		}
		for (int i = 0; i < FUI_WATCHES; i++) {
			struct fui_watch *w = &ui->watches[i];
			if (w->fd >= 0 && w->fd == fds[2 + i].fd && fds[2 + i].revents) {
				w->f(ui, w->fd, w->data);
			}
		}
		if (fds[1].revents & POLLIN) {
			char drain[64];
			while (read(ui->wake[0], drain, sizeof(drain)) > 0) {
			}
			fui_redraw(ui);
		}
		if (fd->revents & POLLIN) {
			if (wl_display_read_events(ui->display) < 0) {
				return 1;
			}
		} else {
			wl_display_cancel_read(ui->display);
		}
		if (wl_display_dispatch_pending(ui->display) < 0) {
			return 1;
		}
		if (ui->dirty) {
			draw(ui);
		}
	}
	return 0;
}

void fui_destroy(struct fui *ui) {
	fui_thumbs_free(ui); // its threads wake the window: gone before it is
	fui_dnd_finish(ui);
	for (int i = 0; i < 2; i++) {
		fui_buffer_free(&ui->buffers[i]);
	}
	fui_fonts_free(ui);
	if (ui->xkb_state) {
		xkb_state_unref(ui->xkb_state);
	}
	if (ui->keymap) {
		xkb_keymap_unref(ui->keymap);
	}
	if (ui->xkb) {
		xkb_context_unref(ui->xkb);
	}
	if (ui->display) {
		wl_display_disconnect(ui->display);
	}
	for (int i = 0; i < 2; i++) {
		if (ui->wake[i] >= 0) {
			close(ui->wake[i]);
		}
	}
	free(ui);
}

bool fui_watch(struct fui *ui, int fd, void (*f)(struct fui *ui, int fd, void *data),
		void *data) {
	for (int i = 0; i < FUI_WATCHES; i++) {
		if (ui->watches[i].fd < 0) {
			ui->watches[i] = (struct fui_watch){fd, f, data};
			return true;
		}
	}
	return false;
}

void fui_unwatch(struct fui *ui, int fd) {
	for (int i = 0; i < FUI_WATCHES; i++) {
		if (ui->watches[i].fd == fd) {
			ui->watches[i].fd = -1;
		}
	}
}

unsigned fui_mods(const struct fui *ui) {
	if (!ui->xkb_state) {
		return 0;
	}
	static const struct {
		const char *name;
		unsigned mod;
	} names[] = {
		{XKB_MOD_NAME_SHIFT, FUI_SHIFT},
		{XKB_MOD_NAME_CTRL, FUI_CTRL},
		{XKB_MOD_NAME_ALT, FUI_ALT},
		{XKB_MOD_NAME_LOGO, FUI_SUPER},
	};
	unsigned mods = 0;
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (xkb_state_mod_name_is_active(ui->xkb_state, names[i].name,
				XKB_STATE_MODS_EFFECTIVE) > 0) {
			mods |= names[i].mod;
		}
	}
	return mods;
}

void fui_wake(struct fui *ui) {
	char byte = 1;
	ssize_t n = write(ui->wake[1], &byte, 1);
	(void)n; // full: it's woken already
}
