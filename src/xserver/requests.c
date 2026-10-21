#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wlr/util/log.h>

#include "xserver/internal.h"

// field offsets below are from the X11 protocol spec: byte 0 is the opcode,
// byte 1 the request's data byte, 2-3 the length, then the fields

static struct xs_window *get_window(struct xs_client *client, const struct xin *in,
		size_t at, uint8_t opcode) {
	uint32_t id = xin32(in, at);
	struct xs_window *window = xserver_window(client->xserver, id);
	if (!window) {
		xs_error(client, XS_ERROR_WINDOW, id, opcode, 0);
	}
	return window;
}

static void reply_empty(struct xs_client *client, uint8_t data) {
	struct xout out;
	xs_reply_begin(client, &out, data);
	xs_reply_send(client, &out);
}

// ---- windows ----

// CreateWindow and ChangeWindowAttributes share their value list
static void apply_attributes(struct xs_client *client, struct xs_window *window,
		uint32_t mask, const struct xin *in, size_t at) {
	for (int bit = 0; bit < 15; bit++) {
		if (!(mask & (1u << bit))) {
			continue;
		}
		uint32_t value = xin32(in, at);
		at += 4;
		switch (bit) {
		case 0: // background pixmap: None, ParentRelative or a tile
			window->background = value == 0 ? XS_BACKGROUND_NONE
				: value == 1 ? XS_BACKGROUND_PARENT : XS_BACKGROUND_PIXMAP;
			window->background_pixmap = value > 1 ? value : 0;
			break;
		case 1:
			window->background = XS_BACKGROUND_PIXEL;
			window->background_pixel = value;
			break;
		case 9:
			window->override_redirect = value & 1;
			break;
		case 11:
			xs_window_set_mask(window, client, value);
			break;
		case 12:
			window->do_not_propagate = (uint16_t)value;
			break;
		case 13:
			window->colormap = value;
			break;
		default:
			break; // borders, gravity, backing store, cursors
		}
	}
}

static void create_window(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	uint32_t id = xin32(in, 4);
	struct xs_window *parent = get_window(client, in, 8, 1);
	if (!parent) {
		return;
	}
	if (!xs_id_available(client, id)) {
		xs_error(client, XS_ERROR_ID_CHOICE, id, 1, 0);
		return;
	}
	struct xs_window *window = xs_window_create(xserver, client, id, parent);
	if (!window) {
		xs_error(client, XS_ERROR_ALLOC, id, 1, 0);
		return;
	}
	uint8_t depth = xin8(in, 1);
	uint16_t class = xin16(in, 22);
	uint32_t visual = xin32(in, 24);
	window->x = xin16s(in, 12);
	window->y = xin16s(in, 14);
	window->width = xin16(in, 16);
	window->height = xin16(in, 18);
	window->border = xin16(in, 20);
	window->class = class ? class : parent->class;
	window->depth = window->class == 2 ? 0 : depth ? depth : parent->depth;
	window->visual = visual ? visual : parent->visual;
	if (window->depth == 32) {
		window->visual = XS_VISUAL_32;
	}
	apply_attributes(client, window, xin32(in, 28), in, 32);

	if (xserver->hooks.window_created) {
		xserver->hooks.window_created(xserver->hooks.data, window);
	}
	struct xs_event event = {0};
	xs_event_put8(&event, 0, XS_EVENT_CREATE_NOTIFY);
	xs_event_put32(&event, 4, parent->id);
	xs_event_put32(&event, 8, window->id);
	xs_event_put16(&event, 12, (uint16_t)window->x);
	xs_event_put16(&event, 14, (uint16_t)window->y);
	xs_event_put16(&event, 16, window->width);
	xs_event_put16(&event, 18, window->height);
	xs_event_put16(&event, 20, window->border);
	xs_event_put8(&event, 22, window->override_redirect);
	xs_event_deliver(xserver, parent, XS_MASK_SUBSTRUCTURE, &event);
	xs_changed(xserver);
}

static void get_window_attributes(struct xs_client *client, const struct xin *in) {
	struct xs_window *window = get_window(client, in, 4, 3);
	if (!window) {
		return;
	}
	struct xout out;
	xs_reply_begin(client, &out, 0); // backing store: never
	xout32(&out, window->visual);
	xout16(&out, window->class);
	xout8(&out, 0); // bit gravity
	xout8(&out, 1); // win gravity: NorthWest
	xout32(&out, 0xffffffff); // backing planes
	xout32(&out, 0); // backing pixel
	xout8(&out, 0); // save under
	xout8(&out, 1); // map installed
	xout8(&out, !window->mapped ? 0 : xs_window_viewable(window) ? 2 : 1);
	xout8(&out, window->override_redirect);
	xout32(&out, window->colormap);
	xout32(&out, xs_window_all_masks(window));
	xout32(&out, xs_window_mask(window, client));
	xout16(&out, window->do_not_propagate);
	xout16(&out, 0);
	xs_reply_send(client, &out);
}

static void configure_window(struct xs_client *client, const struct xin *in) {
	struct xs_window *window = get_window(client, in, 4, 12);
	if (!window) {
		return;
	}
	uint16_t mask = xin16(in, 8);
	int32_t values[7] = {0};
	size_t at = 12;
	for (int bit = 0; bit < 7; bit++) {
		if (mask & (1u << bit)) {
			values[bit] = (int32_t)xin32(in, at);
			at += 4;
		}
	}
	xs_window_configure(client->xserver, window, mask, values[0], values[1], values[2],
		values[3], values[4], (uint32_t)values[5], (uint8_t)values[6]);
}

static void get_geometry(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	uint32_t id = xin32(in, 4);
	struct xs_window *window = xserver_window(xserver, id);
	int16_t x = 0, y = 0;
	uint16_t width, height, border = 0;
	uint8_t depth;
	if (window) {
		x = window->x;
		y = window->y;
		width = window->width;
		height = window->height;
		border = window->border;
		depth = window->depth;
	} else {
		struct xs_pixmap *pixmap, *found = NULL;
		wl_list_for_each(pixmap, &xserver->pixmaps, link) {
			if (pixmap->id == id) {
				found = pixmap;
				break;
			}
		}
		if (!found) {
			xs_error(client, XS_ERROR_DRAWABLE, id, 14, 0);
			return;
		}
		width = found->width;
		height = found->height;
		depth = found->depth;
	}
	struct xout out;
	xs_reply_begin(client, &out, depth);
	xout32(&out, XS_ROOT);
	xout16(&out, (uint16_t)x);
	xout16(&out, (uint16_t)y);
	xout16(&out, width);
	xout16(&out, height);
	xout16(&out, border);
	xs_reply_send(client, &out);
}

static void query_tree(struct xs_client *client, const struct xin *in) {
	struct xs_window *window = get_window(client, in, 4, 15);
	if (!window) {
		return;
	}
	struct xout out;
	xs_reply_begin(client, &out, 0);
	xout32(&out, XS_ROOT);
	xout32(&out, window->parent ? window->parent->id : 0);
	uint16_t count = (uint16_t)wl_list_length(&window->children);
	xout16(&out, count);
	xout_zero(&out, 14);
	struct xs_window *child;
	wl_list_for_each(child, &window->children, link) {
		xout32(&out, child->id);
	}
	xs_reply_send(client, &out);
}

static void translate_coordinates(struct xs_client *client, const struct xin *in) {
	struct xs_window *src = get_window(client, in, 4, 40);
	struct xs_window *dst = src ? get_window(client, in, 8, 40) : NULL;
	if (!dst) {
		return;
	}
	int sx, sy, dx, dy;
	xs_window_origin(src, &sx, &sy);
	xs_window_origin(dst, &dx, &dy);
	int x = sx + xin16s(in, 12), y = sy + xin16s(in, 14);
	struct xs_window *child = xs_window_child_at(dst, x, y);
	struct xout out;
	xs_reply_begin(client, &out, 1); // same screen
	xout32(&out, child ? child->id : 0);
	xout16(&out, (uint16_t)(x - dx));
	xout16(&out, (uint16_t)(y - dy));
	xs_reply_send(client, &out);
}

// ---- atoms and properties ----

static void intern_atom(struct xs_client *client, const struct xin *in) {
	uint16_t len = xin16(in, 4);
	if (8 + (size_t)len > in->len) {
		xs_error(client, XS_ERROR_LENGTH, 0, 16, 0);
		return;
	}
	uint32_t atom = xs_atom_intern(&client->xserver->atoms, (const char *)in->data + 8, len,
		!xin8(in, 1));
	struct xout out;
	xs_reply_begin(client, &out, 0);
	xout32(&out, atom);
	xs_reply_send(client, &out);
}

static void get_atom_name(struct xs_client *client, const struct xin *in) {
	uint32_t atom = xin32(in, 4);
	const char *name = xs_atom_name(&client->xserver->atoms, atom);
	if (!name) {
		xs_error(client, XS_ERROR_ATOM, atom, 17, 0);
		return;
	}
	size_t len = strlen(name);
	struct xout out;
	xs_reply_begin(client, &out, 0);
	xout16(&out, (uint16_t)len);
	xout_zero(&out, 22);
	xout_bytes(&out, name, len);
	xs_reply_send(client, &out);
}

static void change_property(struct xs_client *client, const struct xin *in) {
	struct xs_window *window = get_window(client, in, 4, 18);
	if (!window) {
		return;
	}
	uint8_t mode = xin8(in, 1);
	uint32_t name = xin32(in, 8), type = xin32(in, 12);
	uint8_t format = xin8(in, 16);
	uint32_t units = xin32(in, 20);
	if (format != 8 && format != 16 && format != 32) {
		xs_error(client, XS_ERROR_VALUE, format, 18, 0);
		return;
	}
	if (mode > 2) {
		xs_error(client, XS_ERROR_VALUE, mode, 18, 0);
		return;
	}
	if (!xs_atom_name(&client->xserver->atoms, name)) {
		xs_error(client, XS_ERROR_ATOM, name, 18, 0);
		return;
	}
	size_t bytes = (size_t)units * (format / 8);
	if (24 + bytes > in->len) {
		xs_error(client, XS_ERROR_LENGTH, 0, 18, 0);
		return;
	}
	uint8_t error = 0;
	if (!xs_window_change_property(client->xserver, window, mode, name, type, format, units,
			in->data + 24, &error)) {
		xs_error(client, error, name, 18, 0);
	}
}

static void get_property(struct xs_client *client, const struct xin *in) {
	struct xs_window *window = get_window(client, in, 4, 20);
	if (!window) {
		return;
	}
	bool delete = xin8(in, 1);
	uint32_t name = xin32(in, 8), type = xin32(in, 12);
	uint32_t offset = xin32(in, 16), length = xin32(in, 20);
	struct xs_property *property = xs_window_property(window, name);
	struct xout out;
	if (!property) {
		xs_reply_begin(client, &out, 0);
		xout32(&out, 0); // type None
		xs_reply_send(client, &out);
		return;
	}
	size_t unit = property->format / 8;
	size_t size = (size_t)property->units * unit;
	if (type != 0 && type != property->type) {
		// wrong type: what it is and how big, no data
		xs_reply_begin(client, &out, property->format);
		xout32(&out, property->type);
		xout32(&out, (uint32_t)size);
		xs_reply_send(client, &out);
		return;
	}
	size_t start = (size_t)offset * 4;
	if (start > size) {
		xs_error(client, XS_ERROR_VALUE, offset, 20, 0);
		return;
	}
	size_t count = size - start;
	if ((uint64_t)length * 4 < count) {
		count = (size_t)length * 4;
	}
	size_t after = size - start - count;
	xs_reply_begin(client, &out, property->format);
	xout32(&out, property->type);
	xout32(&out, (uint32_t)after);
	xout32(&out, (uint32_t)(count / unit));
	xout_zero(&out, 12);
	xout_bytes(&out, property->data + start, count);
	xs_reply_send(client, &out);
	if (delete && after == 0) {
		xs_window_delete_property(client->xserver, window, name);
	}
}

static void list_properties(struct xs_client *client, const struct xin *in) {
	struct xs_window *window = get_window(client, in, 4, 21);
	if (!window) {
		return;
	}
	struct xout out;
	xs_reply_begin(client, &out, 0);
	xout16(&out, (uint16_t)wl_list_length(&window->properties));
	xout_zero(&out, 22);
	struct xs_property *property;
	wl_list_for_each(property, &window->properties, link) {
		xout32(&out, property->name);
	}
	xs_reply_send(client, &out);
}

static void send_event(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	uint32_t destination = xin32(in, 4), mask = xin32(in, 8);
	if (in->len < 44) {
		xs_error(client, XS_ERROR_LENGTH, 0, 25, 0);
		return;
	}
	struct xs_window *window;
	if (destination == 0) { // PointerWindow
		double px = 0, py = 0;
		if (xserver->hooks.pointer) {
			xserver->hooks.pointer(xserver->hooks.data, &px, &py);
		}
		window = xs_window_child_at(xserver->root, (int)px, (int)py);
	} else if (destination == 1) { // InputFocus
		window = xserver_window(xserver, xserver->focus);
	} else {
		window = xserver_window(xserver, destination);
		if (!window) {
			xs_error(client, XS_ERROR_WINDOW, destination, 25, 0);
			return;
		}
	}
	if (!window) {
		return;
	}
	struct xs_event event;
	memcpy(event.bytes, in->data + 12, 32);
	event.bytes[0] |= 0x80; // came from SendEvent
	if (mask == 0) {
		// to whoever made the window. flame's own: a selection's answer
		if (window->owner) {
			xs_event_send(window->owner, &event);
		} else if (window->parent) {
			xs_flame_window_event(xserver, window, &event);
		}
		return;
	}
	// messages for the window manager: flame is it. anyone else listening
	// (a pager) hears them too
	if (window == xserver->root && (event.bytes[0] & 0x7f) == 33 && event.bytes[1] == 32
			&& (mask & (XS_MASK_SUBSTRUCTURE | XS_MASK_SUBSTRUCTURE_REDIRECT))
			&& xserver->hooks.wm_message) {
		uint32_t about, type, data[5];
		memcpy(&about, event.bytes + 4, 4);
		memcpy(&type, event.bytes + 8, 4);
		memcpy(data, event.bytes + 12, 20);
		xserver->hooks.wm_message(xserver->hooks.data, about, type, data);
	}
	xs_event_deliver(xserver, window, mask, &event);
}

// ---- input ----

static void query_pointer(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	struct xs_window *window = get_window(client, in, 4, 38);
	if (!window) {
		return;
	}
	double px = 0, py = 0;
	if (xserver->hooks.pointer) {
		xserver->hooks.pointer(xserver->hooks.data, &px, &py);
	}
	int x = (int)px, y = (int)py;
	int wx, wy;
	xs_window_origin(window, &wx, &wy);
	struct xs_window *child = xs_window_child_at(window, x, y);
	struct xout out;
	xs_reply_begin(client, &out, 1); // same screen
	xout32(&out, XS_ROOT);
	xout32(&out, child ? child->id : 0);
	xout16(&out, (uint16_t)x);
	xout16(&out, (uint16_t)y);
	xout16(&out, (uint16_t)(x - wx));
	xout16(&out, (uint16_t)(y - wy));
	xout16(&out, xserver->state);
	xs_reply_send(client, &out);
}

static void set_input_focus(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	uint32_t focus = xin32(in, 4);
	if (focus > XS_FOCUS_POINTER_ROOT) {
		struct xs_window *window = xserver_window(xserver, focus);
		if (!window) {
			xs_error(client, XS_ERROR_WINDOW, focus, 42, 0);
			return;
		}
		if (!xs_window_viewable(window)) {
			xs_error(client, XS_ERROR_MATCH, focus, 42, 0);
			return;
		}
	}
	xs_focus_set(xserver, focus, xin8(in, 1));
	if (xserver->hooks.focus_changed) {
		xserver->hooks.focus_changed(xserver->hooks.data, focus);
	}
}

static void get_input_focus(struct xs_client *client) {
	struct xout out;
	xs_reply_begin(client, &out, client->xserver->focus_revert);
	xout32(&out, client->xserver->focus);
	xs_reply_send(client, &out);
}

// ---- fonts, colors, keyboard: enough for clients that ask on the side ----

struct named_color {
	const char *name;
	uint8_t r, g, b;
};

static const struct named_color named_colors[] = {
	{"black", 0, 0, 0}, {"white", 255, 255, 255}, {"gray", 190, 190, 190},
	{"grey", 190, 190, 190}, {"red", 255, 0, 0}, {"green", 0, 255, 0},
	{"blue", 0, 0, 255}, {"yellow", 255, 255, 0},
};

static const struct named_color *lookup_color(const struct xin *in, size_t name_at,
		uint16_t len) {
	if (name_at + len > in->len) {
		return NULL;
	}
	for (size_t i = 0; i < sizeof(named_colors) / sizeof(named_colors[0]); i++) {
		if (strlen(named_colors[i].name) == len
				&& !strncasecmp(named_colors[i].name, (const char *)in->data + name_at, len)) {
			return &named_colors[i];
		}
	}
	return NULL;
}

static uint32_t pixel_of(uint16_t r, uint16_t g, uint16_t b) {
	return (uint32_t)(r >> 8) << 16 | (uint32_t)(g >> 8) << 8 | (uint32_t)(b >> 8);
}

static void alloc_color(struct xs_client *client, const struct xin *in) {
	uint16_t r = xin16(in, 8), g = xin16(in, 10), b = xin16(in, 12);
	struct xout out;
	xs_reply_begin(client, &out, 0);
	xout16(&out, r & 0xff00);
	xout16(&out, g & 0xff00);
	xout16(&out, b & 0xff00);
	xout16(&out, 0);
	xout32(&out, pixel_of(r, g, b));
	xs_reply_send(client, &out);
}

static void named_color(struct xs_client *client, const struct xin *in, uint8_t opcode) {
	const struct named_color *color = lookup_color(in, 12, xin16(in, 8));
	if (!color) {
		xs_error(client, XS_ERROR_NAME, 0, opcode, 0);
		return;
	}
	uint16_t r = color->r * 257, g = color->g * 257, b = color->b * 257;
	struct xout out;
	xs_reply_begin(client, &out, 0);
	if (opcode == 85) { // AllocNamedColor: pixel first
		xout32(&out, pixel_of(r, g, b));
	}
	for (int i = 0; i < 2; i++) { // exact, then visual
		xout16(&out, r);
		xout16(&out, g);
		xout16(&out, b);
	}
	xs_reply_send(client, &out);
}

static void query_colors(struct xs_client *client, const struct xin *in) {
	size_t count = in->len > 8 ? (in->len - 8) / 4 : 0;
	struct xout out;
	xs_reply_begin(client, &out, 0);
	xout16(&out, (uint16_t)count);
	xout_zero(&out, 22);
	for (size_t i = 0; i < count; i++) {
		uint32_t pixel = xin32(in, 8 + i * 4);
		xout16(&out, (uint16_t)(((pixel >> 16) & 0xff) * 257));
		xout16(&out, (uint16_t)(((pixel >> 8) & 0xff) * 257));
		xout16(&out, (uint16_t)((pixel & 0xff) * 257));
		xout16(&out, 0);
	}
	xs_reply_send(client, &out);
}

// flame's xkb keymap as X's core one: two levels per key, plain and shifted
// flame's keymap, the core way: a row of keysyms per keycode, both layouts
static void get_keyboard_mapping(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	uint8_t first = xin8(in, 4), count = xin8(in, 5);
	if (first < 8 || first + count - 1 > 255) {
		xs_error(client, XS_ERROR_VALUE, first, 101, 0);
		return;
	}
	int width = xs_xkb_core_width(xserver);
	struct xout out;
	xs_reply_begin(client, &out, (uint8_t)width);
	xout_zero(&out, 24);
	for (int i = 0; i < count; i++) {
		uint32_t syms[XS_XKB_CORE_MAX];
		xs_xkb_core_syms(xserver, (uint8_t)(first + i), syms, width);
		for (int j = 0; j < width; j++) {
			xout32(&out, syms[j]);
		}
	}
	xs_reply_send(client, &out);
}

// which keys are the modifiers, from what the keymap's keys do
static void get_modifier_mapping(struct xs_client *client) {
	struct flame_xserver *xserver = client->xserver;
	uint8_t max;
	uint8_t min = xs_xkb_range(xserver, &max);
	uint8_t keys[8][8] = {{0}};
	int counts[8] = {0}, per = 1;
	for (int k = min; k <= max; k++) {
		uint8_t mods = xs_xkb_modmap(xserver, (uint8_t)k);
		for (int m = 0; m < 8; m++) {
			if ((mods & (1u << m)) && counts[m] < 8) {
				keys[m][counts[m]++] = (uint8_t)k;
				per = counts[m] > per ? counts[m] : per;
			}
		}
	}
	struct xout out;
	xs_reply_begin(client, &out, (uint8_t)per); // keycodes per modifier
	xout_zero(&out, 24);
	for (int m = 0; m < 8; m++) {
		xout_bytes(&out, keys[m], (size_t)per);
	}
	xs_reply_send(client, &out);
}

static const struct extension {
	const char *name;
	uint8_t opcode, first_event, first_error;
} extensions[] = {
	{"BIG-REQUESTS", XS_BIG_REQUESTS_OPCODE, 0, 0},
	{"GLX", XS_GLX_OPCODE, XS_GLX_FIRST_EVENT, XS_GLX_FIRST_ERROR},
	{"X-Resource", XS_XRES_OPCODE, 0, 0},
	{"Generic Event Extension", XS_GE_OPCODE, 0, 0},
	{"XFIXES", XS_XFIXES_OPCODE, XS_XFIXES_FIRST_EVENT, XS_XFIXES_FIRST_ERROR},
	{"SYNC", XS_SYNC_OPCODE, XS_SYNC_FIRST_EVENT, XS_SYNC_FIRST_ERROR},
	{"DRI3", XS_DRI3_OPCODE, 0, 0},
	{"Present", XS_PRESENT_OPCODE, 0, 0},
	{"MIT-SHM", XS_SHM_OPCODE, XS_SHM_FIRST_EVENT, XS_SHM_FIRST_ERROR},
	{"RENDER", XS_RENDER_OPCODE, 0, XS_RENDER_FIRST_ERROR},
	{"RANDR", XS_RANDR_OPCODE, XS_RANDR_FIRST_EVENT, XS_RANDR_FIRST_ERROR},
	{"XInputExtension", XS_XI_OPCODE, XS_XI_FIRST_EVENT, XS_XI_FIRST_ERROR},
	{"XKEYBOARD", XS_XKB_OPCODE, XS_XKB_FIRST_EVENT, XS_XKB_FIRST_ERROR},
	{"XC-MISC", XS_XCMISC_OPCODE, 0, 0},
	{"XINERAMA", XS_XINERAMA_OPCODE, 0, 0},
	{"MIT-SCREEN-SAVER", XS_SCREENSAVER_OPCODE, XS_SCREENSAVER_FIRST_EVENT, 0},
	{"DPMS", XS_DPMS_OPCODE, 0, 0},
	{"XTEST", XS_XTEST_OPCODE, 0, 0},
	{"SHAPE", XS_SHAPE_OPCODE, XS_SHAPE_FIRST_EVENT, 0},
	{"DAMAGE", XS_DAMAGE_OPCODE, XS_DAMAGE_FIRST_EVENT, XS_DAMAGE_FIRST_ERROR},
	{"Composite", XS_COMPOSITE_OPCODE, 0, 0},
};

static void query_extension(struct xs_client *client, const struct xin *in) {
	uint16_t len = xin16(in, 4);
	const struct extension *found = NULL;
	for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++) {
		if (strlen(extensions[i].name) == len && 8 + (size_t)len <= in->len
				&& !memcmp(in->data + 8, extensions[i].name, len)) {
			found = &extensions[i];
		}
	}
	if (!found) {
		wlr_log(WLR_DEBUG, "xserver: %s asked for %.*s, there's none", client->comm, (int)len,
			8 + (size_t)len <= in->len ? (const char *)in->data + 8 : "");
	}
	struct xout out;
	xs_reply_begin(client, &out, 0);
	xout8(&out, found != NULL); // present
	xout8(&out, found ? found->opcode : 0);
	xout8(&out, found ? found->first_event : 0);
	xout8(&out, found ? found->first_error : 0);
	xs_reply_send(client, &out);
}

static void list_extensions(struct xs_client *client) {
	struct xout out;
	size_t count = sizeof(extensions) / sizeof(extensions[0]);
	xs_reply_begin(client, &out, (uint8_t)count);
	xout_zero(&out, 24);
	for (size_t i = 0; i < count; i++) {
		size_t len = strlen(extensions[i].name);
		xout8(&out, (uint8_t)len);
		xout_bytes(&out, extensions[i].name, len);
	}
	xs_reply_send(client, &out);
}

static void big_requests(struct xs_client *client, const struct xin *in) {
	if (xin8(in, 1) != 0) {
		xs_error(client, XS_ERROR_REQUEST, 0, XS_BIG_REQUESTS_OPCODE, xin8(in, 1));
		return;
	}
	client->big_requests = true;
	struct xout out;
	xs_reply_begin(client, &out, 0);
	xout32(&out, (16u << 20) / 4); // in units
	xs_reply_send(client, &out);
}

// ---- dispatch ----

void xs_request(struct xs_client *client, uint8_t opcode, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	struct xs_window *window;
	struct xout out;
	if (xserver->trace) {
		wlr_log(WLR_INFO, "xserver: %s #%u: %u.%u, %zu bytes", client->comm,
			(unsigned)client->seq, opcode, opcode >= 128 ? xin8(in, 1) : 0, in->len);
	}
	if (xs_draw_request(client, opcode, in)) {
		return; // GCs, pixmaps and drawing, see draw.c
	}
	switch (opcode) {
	case 1:
		create_window(client, in);
		break;
	case 2: // ChangeWindowAttributes
		if ((window = get_window(client, in, 4, 2))) {
			bool was = window->override_redirect;
			apply_attributes(client, window, xin32(in, 8), in, 12);
			if (was != window->override_redirect) {
				xs_changed(xserver);
			}
		}
		break;
	case 3:
		get_window_attributes(client, in);
		break;
	case 4: // DestroyWindow
		// flame's own windows stay
		if ((window = get_window(client, in, 4, 4)) && window->parent && window->owner) {
			wlr_log(WLR_DEBUG, "xserver: %s destroys 0x%x", client->comm, window->id);
			xs_window_destroy(xserver, window);
		}
		break;
	case 5: // DestroySubwindows: flame's own (on the root) stay
		if ((window = get_window(client, in, 4, 5))) {
			struct xs_window *child, *tmp;
			wl_list_for_each_reverse_safe(child, tmp, &window->children, link) {
				if (child->owner) {
					xs_window_destroy(xserver, child);
				}
			}
		}
		break;
	case 7: // ReparentWindow
		if ((window = get_window(client, in, 4, 7))) {
			struct xs_window *parent = get_window(client, in, 8, 7);
			if (!window->owner) {
				// flame's own windows stay where flame put them
				xs_error(client, XS_ERROR_ACCESS, window->id, 7, 0);
				return;
			}
			if (parent && window->parent) {
				// no loops: the new parent can't be inside the window
				for (struct xs_window *up = parent; up; up = up->parent) {
					if (up == window) {
						xs_error(client, XS_ERROR_MATCH, parent->id, 7, 0);
						return;
					}
				}
				xs_window_reparent(xserver, window, parent, xin16s(in, 12), xin16s(in, 14));
			}
		}
		break;
	case 8: // MapWindow
		if ((window = get_window(client, in, 4, 8))) {
			xs_window_map(xserver, window);
		}
		break;
	case 9: // MapSubwindows
		if ((window = get_window(client, in, 4, 9))) {
			struct xs_window *child;
			wl_list_for_each(child, &window->children, link) {
				xs_window_map(xserver, child);
			}
		}
		break;
	case 10: // UnmapWindow
		if ((window = get_window(client, in, 4, 10))) {
			xs_window_unmap(xserver, window);
		}
		break;
	case 11: // UnmapSubwindows
		if ((window = get_window(client, in, 4, 11))) {
			struct xs_window *child;
			wl_list_for_each(child, &window->children, link) {
				xs_window_unmap(xserver, child);
			}
		}
		break;
	case 12:
		configure_window(client, in);
		break;
	case 14:
		get_geometry(client, in);
		break;
	case 15:
		query_tree(client, in);
		break;
	case 16:
		intern_atom(client, in);
		break;
	case 17:
		get_atom_name(client, in);
		break;
	case 18:
		change_property(client, in);
		break;
	case 19: // DeleteProperty
		if ((window = get_window(client, in, 4, 19))) {
			xs_window_delete_property(xserver, window, xin32(in, 8));
		}
		break;
	case 20:
		get_property(client, in);
		break;
	case 21:
		list_properties(client, in);
		break;
	case 22:
		xs_set_selection_owner(client, in);
		break;
	case 23:
		xs_get_selection_owner(client, in);
		break;
	case 24:
		xs_convert_selection(client, in);
		break;
	case 25:
		send_event(client, in);
		break;
	case 26: // GrabPointer: owner events, window, event mask, modes, confine, cursor, time
		reply_empty(client, xs_grab(xserver, client, false, xin32(in, 4), xin8(in, 1), false,
			xin16(in, 8)));
		break;
	case 27: // UngrabPointer
		xs_ungrab(xserver, client, false);
		break;
	case 31: // GrabKeyboard: owner events, window, time, modes
		reply_empty(client, xs_grab(xserver, client, true, xin32(in, 4), xin8(in, 1), false, 0));
		break;
	case 32: // UngrabKeyboard
		xs_ungrab(xserver, client, true);
		break;
	case 38:
		query_pointer(client, in);
		break;
	case 39: // GetMotionEvents: no history
		xs_reply_begin(client, &out, 0);
		xout32(&out, 0);
		xs_reply_send(client, &out);
		break;
	case 40:
		translate_coordinates(client, in);
		break;
	case 42:
		set_input_focus(client, in);
		break;
	case 43:
		get_input_focus(client);
		break;
	case 44: // QueryKeymap: nothing held
		xs_reply_begin(client, &out, 0);
		xout_zero(&out, 32);
		xs_reply_send(client, &out);
		break;
	case 45: case 46: case 47: case 48: case 49: case 50: case 51: case 52:
		xs_font_request(client, in);
		break;
	case 54: { // FreePixmap
		uint32_t id = xin32(in, 4);
		struct xs_pixmap *pixmap, *tmp;
		wl_list_for_each_safe(pixmap, tmp, &xserver->pixmaps, link) {
			if (pixmap->id == id) {
				xs_pixmap_destroy(xserver, pixmap);
			}
		}
		break;
	}
	case 83: // ListInstalledColormaps
		xs_reply_begin(client, &out, 0);
		xout16(&out, 1);
		xout_zero(&out, 22);
		xout32(&out, XS_COLORMAP);
		xs_reply_send(client, &out);
		break;
	case 84:
		alloc_color(client, in);
		break;
	case 85: // AllocNamedColor
		named_color(client, in, 85);
		break;
	case 86: // AllocColorCells
	case 87: // AllocColorPlanes
		xs_error(client, XS_ERROR_ALLOC, 0, opcode, 0); // TrueColor, nothing to allocate
		break;
	case 91:
		query_colors(client, in);
		break;
	case 92: // LookupColor
		named_color(client, in, 92);
		break;
	case 97: // QueryBestSize: whatever was asked
		xs_reply_begin(client, &out, 0);
		xout16(&out, xin16(in, 8));
		xout16(&out, xin16(in, 10));
		xs_reply_send(client, &out);
		break;
	case 98:
		query_extension(client, in);
		break;
	case 99:
		list_extensions(client);
		break;
	case 101:
		get_keyboard_mapping(client, in);
		break;
	case 103: // GetKeyboardControl
		xs_reply_begin(client, &out, 1); // global auto repeat on
		xout32(&out, 0); // leds
		xout8(&out, 0); // key click
		xout8(&out, 50); // bell percent
		xout16(&out, 400); // bell pitch
		xout16(&out, 100); // bell duration
		xout16(&out, 0);
		xout_zero(&out, 32); // auto repeats
		xs_reply_send(client, &out);
		break;
	case 106: // GetPointerControl
		xs_reply_begin(client, &out, 0);
		xout16(&out, 2);
		xout16(&out, 1);
		xout16(&out, 4);
		xs_reply_send(client, &out);
		break;
	case 108: // GetScreenSaver: off
		reply_empty(client, 0);
		break;
	case 110: // ListHosts
		xs_reply_begin(client, &out, 0);
		xout16(&out, 0);
		xs_reply_send(client, &out);
		break;
	case 116: // SetPointerMapping
	case 118: // SetModifierMapping
		reply_empty(client, 0); // Success
		break;
	case 117: // GetPointerMapping: 1 2 3
		xs_reply_begin(client, &out, 3);
		xout_zero(&out, 24);
		xout8(&out, 1);
		xout8(&out, 2);
		xout8(&out, 3);
		xs_reply_send(client, &out);
		break;
	case 119:
		get_modifier_mapping(client);
		break;
	case XS_BIG_REQUESTS_OPCODE:
		big_requests(client, in);
		break;
	case XS_GLX_OPCODE:
		xs_glx_request(client, in);
		break;
	case XS_XRES_OPCODE:
		xs_xres_request(client, in);
		break;
	case XS_GE_OPCODE: // QueryVersion is all there is: 1.0
		xs_reply_begin(client, &out, 0);
		xout16(&out, 1);
		xout16(&out, 0);
		xs_reply_send(client, &out);
		break;
	case XS_XFIXES_OPCODE:
		xs_xfixes_request(client, in);
		break;
	case XS_SYNC_OPCODE:
		xs_sync_request(client, in);
		break;
	case XS_DRI3_OPCODE:
		xs_dri3_request(client, in);
		break;
	case XS_PRESENT_OPCODE:
		xs_present_request(client, in);
		break;
	case XS_SHM_OPCODE:
		xs_shm_request(client, in);
		break;
	case XS_RENDER_OPCODE:
		xs_render_request(client, in);
		break;
	case XS_RANDR_OPCODE:
		xs_randr_request(client, in);
		break;
	case XS_XI_OPCODE:
		xs_xi_request(client, in);
		break;
	case XS_XKB_OPCODE:
		xs_xkb_request(client, in);
		break;
	case XS_XCMISC_OPCODE:
		xs_xcmisc_request(client, in);
		break;
	case XS_XINERAMA_OPCODE:
		xs_xinerama_request(client, in);
		break;
	case XS_SCREENSAVER_OPCODE:
		xs_screensaver_request(client, in);
		break;
	case XS_DPMS_OPCODE:
		xs_dpms_request(client, in);
		break;
	case XS_XTEST_OPCODE:
		xs_xtest_request(client, in);
		break;
	case XS_SHAPE_OPCODE:
		xs_shape_request(client, in);
		break;
	case XS_DAMAGE_OPCODE:
		xs_damage_request(client, in);
		break;
	case XS_COMPOSITE_OPCODE:
		xs_composite_request(client, in);
		break;
	// accepted, nothing to do: grabs letting go, cursors, colormaps,
	// keyboard and pointer settings, server grabs
	case 6: case 13: case 28: case 29: case 30: case 33: case 34:
	case 35: case 36: case 37: case 41: case 78: case 79: case 80: case 81: case 82: case 88: case 89:
	case 90: case 93: case 94: case 95: case 96: case 100: case 102: case 104: case 105:
	case 107: case 109: case 111: case 112: case 113: case 114: case 115: case 127:
		break;
	default:
		// extensions we never announced, nobody should be sending these
		wlr_log(WLR_DEBUG, "xserver: %s sent unknown request %u", client->comm, opcode);
		xs_error(client, XS_ERROR_REQUEST, 0, opcode, 0);
		break;
	}
}
