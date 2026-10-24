#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "xserver/internal.h"

// XInput 2.2, what toolkits want before they'll start (gtk4, winit) and what
// gives them smooth scrolling (gtk3, firefox, qt) and games their raw mouse
// (sdl). the devices: the virtual core pointer and keyboard, with flame's
// pointer and keyboard attached. events come from the masters with the
// slaves as their source. a client that selected an XI2 event on a window
// gets that one instead of the core event there, like Xorg does it. grabs
// are accepted and do nothing, the same as the core ones: flame decides
// who gets input, and a held button keeps it where it went down

#define MAJOR 2
#define MINOR 2

#define MASTER_POINTER 2
#define MASTER_KEYBOARD 3
#define SLAVE_POINTER 4
#define SLAVE_KEYBOARD 5

#define ALL_DEVICES 0
#define ALL_MASTER_DEVICES 1

#define BAD_DEVICE (XS_XI_FIRST_ERROR + 0)

// device uses
#define MASTER_POINTER_USE 1
#define MASTER_KEYBOARD_USE 2
#define SLAVE_POINTER_USE 3
#define SLAVE_KEYBOARD_USE 4

// XI1's GetExtensionVersion, libXi asks it before anything else
#define GET_EXTENSION_VERSION 1

#define BUTTONS XS_POINTER_BUTTONS
// x, y, then scrolling: vertical, horizontal
#define VALUATORS 4
#define SCROLL_INCREMENT 15 // a wheel notch, in wayland's axis units

#define POINTER_EMULATED (1u << 16)

// who wants which XI2 events on a window: from the masters, from the slaves.
// bit n is event type n
struct select {
	struct wl_list link; // flame_xserver.xi_selects
	struct xs_client *client;
	uint32_t window;
	uint32_t master, slave;
};

static struct select *find_select(struct flame_xserver *xserver, struct xs_client *client,
		uint32_t window) {
	struct select *select;
	wl_list_for_each(select, &xserver->xi_selects, link) {
		if (select->client == client && select->window == window) {
			return select;
		}
	}
	return NULL;
}

bool xs_xi_selected(struct flame_xserver *xserver, struct xs_window *window, int evtype) {
	struct select *select;
	wl_list_for_each(select, &xserver->xi_selects, link) {
		if (select->window == window->id && ((select->master | select->slave) >> evtype & 1)) {
			return true;
		}
	}
	return false;
}

bool xs_xi_client_selected(struct xs_client *client, struct xs_window *window, int evtype) {
	if (evtype < 0) {
		return false; // a core event XI2 doesn't stand in for
	}
	struct select *select = find_select(client->xserver, client, window->id);
	return select && ((select->master | select->slave) >> evtype & 1);
}

// ---- events ----

static void put16(uint8_t *at, uint16_t v) {
	memcpy(at, &v, 2);
}

static void put32(uint8_t *at, uint32_t v) {
	memcpy(at, &v, 4);
}

static uint32_t fp1616(double v) {
	return (uint32_t)(int32_t)lround(v * 65536.0);
}

static void put_fp3232(uint8_t *at, double v) {
	double integral = floor(v);
	put32(at, (uint32_t)(int32_t)integral);
	put32(at + 4, (uint32_t)((v - integral) * 4294967296.0));
}

// a GenericEvent of len bytes (32 and up, whole units of 4), zeroed
static void header(uint8_t *event, size_t len, uint16_t evtype, uint16_t device,
		uint32_t time) {
	memset(event, 0, len);
	event[0] = 35;
	event[1] = XS_XI_OPCODE;
	put32(event + 4, (uint32_t)((len - 32) / 4));
	put16(event + 8, evtype);
	put16(event + 10, device);
	put32(event + 12, time);
}

static void send(struct xs_client *client, uint8_t *event, size_t len) {
	memcpy(event + 2, &client->seq, 2);
	xs_client_send(client, event, len);
}

// to every client selecting evtype on window: the master's version to those
// who want masters, the slave's to those who want slaves. device sits at 10
static void deliver(struct flame_xserver *xserver, uint32_t window, int evtype,
		uint8_t *event, size_t len, uint16_t master, uint16_t slave, struct xs_client *only) {
	struct select *select;
	wl_list_for_each(select, &xserver->xi_selects, link) {
		if (select->window != window || (only && select->client != only)) {
			continue;
		}
		if (select->master >> evtype & 1) {
			put16(event + 10, master);
			send(select->client, event, len);
		}
		if (select->slave >> evtype & 1) {
			put16(event + 10, slave);
			send(select->client, event, len);
		}
	}
}

// modifiers and group from a core state: caps and num lock count as locked
static void put_mods(uint8_t *at, uint16_t state) {
	uint32_t effective = state & 0xff;
	uint32_t locked = effective & (2 | 16);
	put32(at, effective & ~locked); // base
	put32(at + 4, 0); // latched
	put32(at + 8, locked);
	put32(at + 12, effective);
	uint8_t group = (uint8_t)((state >> 13) & 3);
	at[16] = group; // base
	at[17] = 0;
	at[18] = 0;
	at[19] = group; // effective
}

// the buttons held, a mask of BUTTONS bits from the core state's 1 to 5
static uint32_t buttons_of(uint16_t state) {
	return (uint32_t)((state >> 8) & 0x1f) << 1;
}

// an event to who should get it: a grab's client on the grab window as the
// master device whatever it selected (forced), or the selections, maybe
// just one client's
static void to(struct flame_xserver *xserver, uint32_t window, int evtype, uint8_t *event,
		size_t len, bool key, const struct xs_input *how) {
	uint16_t master = key ? MASTER_KEYBOARD : MASTER_POINTER;
	uint16_t slave = key ? SLAVE_KEYBOARD : SLAVE_POINTER;
	if (how && how->forced) {
		put16(event + 10, master);
		send(how->only, event, len);
		return;
	}
	deliver(xserver, window, evtype, event, len, master, slave, how ? how->only : NULL);
}

void xs_xi_device_event(struct flame_xserver *xserver, struct xs_window *receiver,
		struct xs_window *source, const struct xs_input *input) {
	bool key = input->evtype == XS_XI_KEY_PRESS || input->evtype == XS_XI_KEY_RELEASE;
	int rx = xserver->pointer_x, ry = xserver->pointer_y;
	int ox, oy;
	xs_window_origin(receiver, &ox, &oy);
	uint32_t child = 0;
	for (struct xs_window *w = source; w && w != receiver; w = w->parent) {
		if (w->parent == receiver) {
			child = w->id;
		}
	}
	// valuators: x and y on every pointer event, scrolling when it scrolled
	uint32_t valuator_mask = 0;
	double values[VALUATORS];
	int count = 0;
	if (!key) {
		valuator_mask = 1 | 2;
		values[count++] = rx;
		values[count++] = ry;
		if (input->scroll & 1) {
			valuator_mask |= 4;
			values[count++] = xserver->scroll[0];
		}
		if (input->scroll & 2) {
			valuator_mask |= 8;
			values[count++] = xserver->scroll[1];
		}
	}
	uint8_t event[80 + 4 + 4 + VALUATORS * 8];
	size_t len = 80 + 4 + 4 + (size_t)count * 8;
	header(event, len, (uint16_t)input->evtype, 0, input->time);
	put32(event + 16, input->detail);
	put32(event + 20, XS_ROOT);
	put32(event + 24, receiver->id);
	put32(event + 28, child);
	put32(event + 32, fp1616(rx));
	put32(event + 36, fp1616(ry));
	put32(event + 40, fp1616(rx - ox));
	put32(event + 44, fp1616(ry - oy));
	put16(event + 48, 1); // buttons, in units
	put16(event + 50, 1); // valuators
	put16(event + 52, key ? SLAVE_KEYBOARD : SLAVE_POINTER);
	put32(event + 56, input->flags);
	put_mods(event + 60, input->state);
	put32(event + 80, buttons_of(input->state));
	put32(event + 84, valuator_mask);
	for (int i = 0; i < count; i++) {
		put_fp3232(event + 88 + i * 8, values[i]);
	}
	to(xserver, receiver->id, input->evtype, event, len, key, input);
}

// Enter, Leave, FocusIn and FocusOut share a layout
static void crossing(struct flame_xserver *xserver, int evtype, struct xs_window *window,
		uint8_t detail, struct xs_window *child, uint16_t state, uint32_t time, bool focus,
		uint8_t mode, const struct xs_input *how) {
	int rx = xserver->pointer_x, ry = xserver->pointer_y;
	int ox, oy;
	xs_window_origin(window, &ox, &oy);
	uint8_t event[76];
	header(event, sizeof(event), (uint16_t)evtype, 0, time);
	put16(event + 16, focus ? SLAVE_KEYBOARD : SLAVE_POINTER);
	event[18] = mode; // Normal, Grab, Ungrab, WhileGrabbed: core's numbers
	event[19] = detail;
	put32(event + 20, XS_ROOT);
	put32(event + 24, window->id);
	put32(event + 28, child ? child->id : 0);
	put32(event + 32, fp1616(rx));
	put32(event + 36, fp1616(ry));
	put32(event + 40, fp1616(rx - ox));
	put32(event + 44, fp1616(ry - oy));
	event[48] = 1; // same screen
	event[49] = xs_focus_within(xserver, window);
	put16(event + 50, 1); // buttons, in units
	put_mods(event + 52, state);
	put32(event + 72, buttons_of(state));
	to(xserver, window->id, evtype, event, sizeof(event), focus, how);
}

void xs_xi_crossing(struct flame_xserver *xserver, int evtype, struct xs_window *window,
		uint8_t detail, struct xs_window *child, uint16_t state, uint32_t time, uint8_t mode,
		const struct xs_input *how) {
	crossing(xserver, evtype, window, detail, child, state, time, false, mode, how);
}

void xs_xi_focus(struct flame_xserver *xserver, int evtype, struct xs_window *window,
		uint8_t detail, uint8_t mode) {
	crossing(xserver, evtype, window, detail, NULL, xserver->state, xs_now(xserver), true,
		mode, NULL);
}

void xs_xi_raw(struct flame_xserver *xserver, int evtype, uint32_t detail, double dx,
		double dy, double raw_dx, double raw_dy, uint32_t time) {
	bool motion = evtype == XS_XI_RAW_MOTION;
	bool key = evtype == XS_XI_RAW_KEY_PRESS || evtype == XS_XI_RAW_KEY_RELEASE;
	int count = motion ? 2 : 0;
	uint8_t event[32 + 4 + 2 * 2 * 8];
	size_t len = 32 + 4 + (size_t)count * 16;
	header(event, len, (uint16_t)evtype, 0, time);
	put32(event + 16, detail);
	put16(event + 20, key ? SLAVE_KEYBOARD : SLAVE_POINTER);
	put16(event + 22, 1); // valuators, in units
	put32(event + 32, motion ? 1 | 2 : 0);
	if (motion) {
		put_fp3232(event + 36, dx);
		put_fp3232(event + 44, dy);
		put_fp3232(event + 52, raw_dx);
		put_fp3232(event + 60, raw_dy);
	}
	// raw events go to whoever asked on the root
	deliver(xserver, XS_ROOT, evtype, event, len, key ? MASTER_KEYBOARD : MASTER_POINTER,
		key ? SLAVE_KEYBOARD : SLAVE_POINTER, NULL);
}

// ---- devices ----

static const char *const button_labels[BUTTONS] = {
	"Button Left", "Button Middle", "Button Right", "Button Wheel Up", "Button Wheel Down",
	"Button Horiz Wheel Left", "Button Horiz Wheel Right", "Button Side", "Button Extra",
};

static const char *const valuator_labels[VALUATORS] = {
	"Rel X", "Rel Y", "Rel Vert Scroll", "Rel Horiz Scroll",
};

static uint32_t atom(struct flame_xserver *xserver, const char *name) {
	return xs_atom_intern(&xserver->atoms, name, strlen(name), true);
}

static void put_valuator(struct xout *out, struct flame_xserver *xserver, uint16_t source,
		int number, double max, double value) {
	xout16(out, 2); // XIValuatorClass
	xout16(out, 11);
	xout16(out, source);
	xout16(out, (uint16_t)number);
	xout32(out, atom(xserver, valuator_labels[number]));
	uint8_t fp[24];
	put_fp3232(fp, 0);
	put_fp3232(fp + 8, max);
	put_fp3232(fp + 16, value);
	xout_bytes(out, fp, sizeof(fp));
	xout32(out, 0); // resolution
	xout8(out, 0); // relative
	xout_zero(out, 3);
}

static void put_scroll(struct xout *out, uint16_t source, int number, uint16_t type) {
	xout16(out, 3); // XIScrollClass
	xout16(out, 6);
	xout16(out, source);
	xout16(out, (uint16_t)number);
	xout16(out, type);
	xout16(out, 0);
	xout32(out, 0); // flags
	uint8_t fp[8];
	put_fp3232(fp, SCROLL_INCREMENT);
	xout_bytes(out, fp, sizeof(fp));
}

// an XIDeviceInfo with its classes, as XIQueryDevice has them
static void put_device(struct xout *out, struct flame_xserver *xserver, uint16_t id) {
	static const struct {
		uint16_t id, use, attachment;
		const char *name;
	} devices[] = {
		{MASTER_POINTER, MASTER_POINTER_USE, MASTER_KEYBOARD, "Virtual core pointer"},
		{MASTER_KEYBOARD, MASTER_KEYBOARD_USE, MASTER_POINTER, "Virtual core keyboard"},
		{SLAVE_POINTER, SLAVE_POINTER_USE, MASTER_POINTER, "flame pointer"},
		{SLAVE_KEYBOARD, SLAVE_KEYBOARD_USE, MASTER_KEYBOARD, "flame keyboard"},
	};
	const char *name = devices[id - MASTER_POINTER].name;
	bool pointer = id == MASTER_POINTER || id == SLAVE_POINTER;
	uint16_t source = pointer ? SLAVE_POINTER : SLAVE_KEYBOARD;
	size_t name_len = strlen(name);
	xout16(out, id);
	xout16(out, devices[id - MASTER_POINTER].use);
	xout16(out, devices[id - MASTER_POINTER].attachment);
	xout16(out, pointer ? 1 + VALUATORS + 2 : 1); // classes
	xout16(out, (uint16_t)name_len);
	xout8(out, 1); // enabled
	xout8(out, 0);
	xout_bytes(out, name, name_len);
	xout_pad(out);
	if (!pointer) {
		xout16(out, 0); // XIKeyClass
		xout16(out, 2 + 248);
		xout16(out, source);
		xout16(out, 248);
		for (uint32_t keycode = 8; keycode < 256; keycode++) {
			xout32(out, keycode);
		}
		return;
	}
	xout16(out, 1); // XIButtonClass
	xout16(out, 2 + 1 + BUTTONS);
	xout16(out, source);
	xout16(out, BUTTONS);
	xout32(out, buttons_of(xserver->state));
	for (int i = 0; i < BUTTONS; i++) {
		xout32(out, atom(xserver, button_labels[i]));
	}
	struct xs_window *root = xserver->root;
	put_valuator(out, xserver, source, 0, root->width - 1, xserver->pointer_x);
	put_valuator(out, xserver, source, 1, root->height - 1, xserver->pointer_y);
	put_valuator(out, xserver, source, 2, 0, xserver->scroll[0]);
	put_valuator(out, xserver, source, 3, 0, xserver->scroll[1]);
	put_scroll(out, source, 2, 1); // vertical
	put_scroll(out, source, 3, 2); // horizontal
}

static bool device_exists(uint16_t id) {
	return id >= MASTER_POINTER && id <= SLAVE_KEYBOARD;
}

// ---- requests ----

static void get_extension_version(struct xs_client *client) {
	struct xout out;
	xs_reply_begin(client, &out, GET_EXTENSION_VERSION);
	xout16(&out, MAJOR);
	xout16(&out, MINOR);
	xout8(&out, 1); // present
	xs_reply_send(client, &out);
}

// XI1's list, chromium and older apps ask it: the same four devices
static void list_input_devices(struct xs_client *client) {
	struct flame_xserver *xserver = client->xserver;
	static const struct {
		const char *name, *type;
		uint8_t use;
	} devices[] = {
		{"Virtual core pointer", "MOUSE", 0}, // IsXPointer
		{"Virtual core keyboard", "KEYBOARD", 1}, // IsXKeyboard
		{"flame pointer", "MOUSE", 4}, // IsXExtensionPointer
		{"flame keyboard", "KEYBOARD", 3}, // IsXExtensionKeyboard
	};
	struct xout out;
	xs_reply_begin(client, &out, 2); // ListInputDevices
	xout8(&out, 4);
	xout_zero(&out, 23);
	for (int i = 0; i < 4; i++) {
		bool pointer = devices[i].use == 0 || devices[i].use == 4;
		xout32(&out, atom(xserver, devices[i].type));
		xout8(&out, (uint8_t)(MASTER_POINTER + i));
		xout8(&out, pointer ? 2 : 1); // classes
		xout8(&out, devices[i].use);
		xout8(&out, 0);
	}
	for (int i = 0; i < 4; i++) {
		bool pointer = devices[i].use == 0 || devices[i].use == 4;
		if (pointer) {
			xout8(&out, 1); // ButtonClass
			xout8(&out, 4);
			xout16(&out, BUTTONS);
			// the same axes XI2 tells about the same pointer (x, y, the two
			// scrolls): chromium puts both lists together, a pointer with two
			// axes here and four there trips it
			xout8(&out, 2); // ValuatorClass
			xout8(&out, 8 + 12 * VALUATORS);
			xout8(&out, VALUATORS); // axes
			xout8(&out, 0); // relative
			xout32(&out, 0); // motion buffer
			for (int axis = 0; axis < VALUATORS; axis++) {
				xout32(&out, 1); // resolution
				xout32(&out, 0);
				xout32(&out, axis == 0 ? xserver->root->width : axis == 1
					? xserver->root->height : 0);
			}
		} else {
			xout8(&out, 0); // KeyClass
			xout8(&out, 8);
			xout8(&out, 8);
			xout8(&out, 255);
			xout16(&out, 248);
			xout16(&out, 0);
		}
	}
	for (int i = 0; i < 4; i++) {
		size_t len = strlen(devices[i].name);
		xout8(&out, (uint8_t)len);
		xout_bytes(&out, devices[i].name, len);
	}
	// at least one byte of padding after the names, then to 4, like Xorg:
	// chromium reads one more byte there and aborts when the reply ends
	// right after the names (every CEF and electron app on X)
	xout8(&out, 0);
	xout_pad(&out);
	xs_reply_send(client, &out);
}

static void query_version(struct xs_client *client, const struct xin *in) {
	uint16_t major = xin16(in, 4), minor = xin16(in, 6);
	if (major > MAJOR || (major == MAJOR && minor > MINOR)) {
		major = MAJOR;
		minor = MINOR;
	}
	struct xout out;
	xs_reply_begin(client, &out, 0);
	xout16(&out, major);
	xout16(&out, minor);
	xs_reply_send(client, &out);
}

static void query_device(struct xs_client *client, const struct xin *in) {
	uint16_t id = xin16(in, 4);
	if (id != ALL_DEVICES && id != ALL_MASTER_DEVICES && !device_exists(id)) {
		xs_error(client, BAD_DEVICE, id, XS_XI_OPCODE, 48);
		return;
	}
	uint16_t ids[4];
	uint16_t count = 0;
	for (uint16_t each = MASTER_POINTER; each <= SLAVE_KEYBOARD; each++) {
		bool master = each <= MASTER_KEYBOARD;
		if (id == ALL_DEVICES || id == each || (id == ALL_MASTER_DEVICES && master)) {
			ids[count++] = each;
		}
	}
	struct xout out;
	xs_reply_begin(client, &out, 0);
	xout16(&out, count);
	xout_zero(&out, 22);
	for (uint16_t i = 0; i < count; i++) {
		put_device(&out, client->xserver, ids[i]);
	}
	xs_reply_send(client, &out);
}

static void select_events(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	uint32_t window = xin32(in, 4);
	uint16_t masks = xin16(in, 8);
	if (!xserver_window(xserver, window)) {
		xs_error(client, XS_ERROR_WINDOW, window, XS_XI_OPCODE, 46);
		return;
	}
	uint32_t master = 0, slave = 0;
	bool master_set = false, slave_set = false;
	size_t at = 12;
	for (uint16_t i = 0; i < masks; i++) {
		if (at + 4 > in->len) {
			xs_error(client, XS_ERROR_LENGTH, 0, XS_XI_OPCODE, 46);
			return;
		}
		uint16_t device = xin16(in, at), units = xin16(in, at + 2);
		if (at + 4 + (size_t)units * 4 > in->len) {
			xs_error(client, XS_ERROR_LENGTH, 0, XS_XI_OPCODE, 46);
			return;
		}
		if (device != ALL_DEVICES && device != ALL_MASTER_DEVICES && !device_exists(device)) {
			xs_error(client, BAD_DEVICE, device, XS_XI_OPCODE, 46);
			return;
		}
		uint32_t bits = units ? xin32(in, at + 4) : 0;
		// a mask for one device sets that device's events, all others keep theirs
		if (device == ALL_DEVICES || device == ALL_MASTER_DEVICES || device <= MASTER_KEYBOARD) {
			master = bits | (master_set ? master : 0);
			master_set = true;
		}
		if (device == ALL_DEVICES || device > MASTER_KEYBOARD) {
			slave = bits | (slave_set ? slave : 0);
			slave_set = true;
		}
		at += 4 + (size_t)units * 4;
	}
	struct select *select = find_select(xserver, client, window);
	if (!select) {
		if (!master && !slave) {
			return;
		}
		select = calloc(1, sizeof(*select));
		if (!select) {
			xs_error(client, XS_ERROR_ALLOC, 0, XS_XI_OPCODE, 46);
			return;
		}
		select->client = client;
		select->window = window;
		wl_list_insert(&xserver->xi_selects, &select->link);
	}
	if (master_set) {
		select->master = master;
	}
	if (slave_set) {
		select->slave = slave;
	}
	if (!select->master && !select->slave) {
		wl_list_remove(&select->link);
		free(select);
	}
}

static void get_selected_events(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	uint32_t window = xin32(in, 4);
	if (!xserver_window(xserver, window)) {
		xs_error(client, XS_ERROR_WINDOW, window, XS_XI_OPCODE, 60);
		return;
	}
	struct select *select = find_select(xserver, client, window);
	struct xout out;
	xs_reply_begin(client, &out, 0);
	uint16_t count = select ? (select->master != 0) + (select->slave != 0) : 0;
	xout16(&out, count);
	xout_zero(&out, 22);
	if (select && select->master) {
		xout16(&out, ALL_MASTER_DEVICES);
		xout16(&out, 1);
		xout32(&out, select->master);
	}
	if (select && select->slave) {
		xout16(&out, SLAVE_POINTER);
		xout16(&out, 1);
		xout32(&out, select->slave);
	}
	xs_reply_send(client, &out);
}

static void query_pointer(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	uint32_t id = xin32(in, 4);
	struct xs_window *window = xserver_window(xserver, id);
	if (!window) {
		xs_error(client, XS_ERROR_WINDOW, id, XS_XI_OPCODE, 40);
		return;
	}
	int x, y;
	xs_pointer_position(xserver, &x, &y);
	int wx, wy;
	xs_window_origin(window, &wx, &wy);
	struct xs_window *child = xs_window_child_at(window, x, y);
	struct xout out;
	xs_reply_begin(client, &out, 0);
	xout32(&out, XS_ROOT);
	xout32(&out, child ? child->id : 0);
	xout32(&out, fp1616(x));
	xout32(&out, fp1616(y));
	xout32(&out, fp1616(x - wx));
	xout32(&out, fp1616(y - wy));
	xout8(&out, 1); // same screen
	xout8(&out, 0);
	xout16(&out, 1); // buttons, in units
	uint8_t mods[20];
	put_mods(mods, xserver->state);
	xout_bytes(&out, mods, sizeof(mods));
	xout32(&out, buttons_of(xserver->state));
	xs_reply_send(client, &out);
}

// XIWarpPointer: src, dst, src x, y (16.16), width, height, dst x, y
// (16.16), device. X's pointer is whole pixels
static void warp_pointer(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	struct xs_window *src = NULL, *dst = NULL;
	uint32_t src_id = xin32(in, 4), dst_id = xin32(in, 8);
	if (src_id && !(src = xserver_window(xserver, src_id))) {
		xs_error(client, XS_ERROR_WINDOW, src_id, XS_XI_OPCODE, 41);
		return;
	}
	if (dst_id && !(dst = xserver_window(xserver, dst_id))) {
		xs_error(client, XS_ERROR_WINDOW, dst_id, XS_XI_OPCODE, 41);
		return;
	}
	xs_warp_request(client, src, (int32_t)xin32(in, 12) >> 16, (int32_t)xin32(in, 16) >> 16,
		xin16(in, 20), xin16(in, 22), dst, (int32_t)xin32(in, 24) >> 16,
		(int32_t)xin32(in, 28) >> 16);
}

static void get_property(struct xs_client *client) {
	struct xout out;
	xs_reply_begin(client, &out, 0);
	xout32(&out, 0); // type None
	xout32(&out, 0); // bytes after
	xout32(&out, 0); // items
	xout8(&out, 0); // format
	xs_reply_send(client, &out);
}

// ---- XInput 1 ----

// what libXi's XI1 calls get, the way Xorg answers them. wine's desktop
// process asks for the button mapping and dies on an error, taking the
// clipping and the foreground window down with it. XI1's events never
// come (XI2 has replaced them), so its grabs, focus and feedback have
// nothing to change

static bool is_pointer(uint8_t device) {
	return device == MASTER_POINTER || device == SLAVE_POINTER;
}

// the device the request is about exists and is the kind asked: BadDevice,
// BadMatch otherwise
static bool xi1_device(struct xs_client *client, uint8_t device, uint8_t minor, int pointer) {
	if (!device_exists(device)) {
		xs_error(client, BAD_DEVICE, device, XS_XI_OPCODE, minor);
		return false;
	}
	if (pointer >= 0 && is_pointer(device) != (pointer == 1)) {
		xs_error(client, XS_ERROR_MATCH, device, XS_XI_OPCODE, minor);
		return false;
	}
	return true;
}

#define ANY_DEVICE (-1)
#define POINTERS 1
#define KEYBOARDS 0

// a reply with just a status (or a count) in its first byte
static void xi1_status(struct xs_client *client, uint8_t minor, uint8_t status) {
	struct xout out;
	xs_reply_begin(client, &out, minor);
	xout8(&out, status);
	xout_zero(&out, 23);
	xs_reply_send(client, &out);
}

static void query_device_state(struct xs_client *client, uint8_t device) {
	struct flame_xserver *xserver = client->xserver;
	struct xout out;
	xs_reply_begin(client, &out, 30);
	xout8(&out, is_pointer(device) ? 2 : 1); // classes
	xout_zero(&out, 23);
	if (is_pointer(device)) {
		uint8_t buttons[32] = {0};
		for (int b = 1; b <= 5; b++) {
			if (xserver->state & (1u << (7 + b))) {
				buttons[b / 8] |= (uint8_t)(1u << (b % 8));
			}
		}
		xout8(&out, 1); // ButtonState
		xout8(&out, 36);
		xout8(&out, BUTTONS);
		xout8(&out, 0);
		xout_bytes(&out, buttons, sizeof(buttons));
		int x, y;
		xs_pointer_position(xserver, &x, &y);
		xout8(&out, 2); // ValuatorState
		xout8(&out, 4 + 4 * VALUATORS);
		xout8(&out, VALUATORS);
		xout8(&out, 0); // relative
		for (int axis = 0; axis < VALUATORS; axis++) {
			xout32(&out, (uint32_t)(axis == 0 ? x : axis == 1 ? y : 0));
		}
	} else {
		xout8(&out, 0); // KeyState: what's held, as QueryKeymap says
		xout8(&out, 36);
		xout8(&out, 248);
		xout8(&out, 0);
		xout_bytes(&out, xserver->keys_down, sizeof(xserver->keys_down));
	}
	xs_reply_send(client, &out);
}

// GetDeviceControl: control, device. resolution for pointers, enabled for all
static void get_device_control(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	uint16_t control = xin16(in, 4);
	uint8_t device = xin8(in, 6);
	if (!xi1_device(client, device, 34, control == 1 ? POINTERS : ANY_DEVICE)) {
		return;
	}
	struct xout out;
	if (control == 1) { // DEVICE_RESOLUTION
		xs_reply_begin(client, &out, 34);
		xout8(&out, 0); // Success
		xout_zero(&out, 23);
		xout16(&out, 1);
		xout16(&out, (uint16_t)(8 + 12 * VALUATORS));
		xout32(&out, VALUATORS);
		for (int axis = 0; axis < VALUATORS; axis++) {
			xout32(&out, 1);
		}
		for (int axis = 0; axis < VALUATORS; axis++) {
			xout32(&out, 0);
		}
		for (int axis = 0; axis < VALUATORS; axis++) {
			xout32(&out, (uint32_t)(axis == 0 ? xserver->root->width : axis == 1
				? xserver->root->height : 0));
		}
		xs_reply_send(client, &out);
	} else if (control == 5) { // DEVICE_ENABLE
		xs_reply_begin(client, &out, 34);
		xout8(&out, 0);
		xout_zero(&out, 23);
		xout16(&out, 5);
		xout16(&out, 8);
		xout8(&out, 1); // enabled
		xout_zero(&out, 3);
		xs_reply_send(client, &out);
	} else {
		xs_error(client, XS_ERROR_VALUE, control, XS_XI_OPCODE, 34);
	}
}

// true: an XI1 request, answered
static bool xi1_request(struct xs_client *client, const struct xin *in, uint8_t minor) {
	struct flame_xserver *xserver = client->xserver;
	struct xout out;
	switch (minor) {
	case 5: // SetDeviceMode: device, mode. relative is all there is
		if (xi1_device(client, xin8(in, 4), minor, POINTERS)) {
			if (xin8(in, 5) != 0) {
				xs_error(client, XS_ERROR_MATCH, xin8(in, 5), XS_XI_OPCODE, minor);
			} else {
				xi1_status(client, minor, 0);
			}
		}
		return true;
	case 7: // GetSelectedExtensionEvents: window. nobody, XI1's events never come
	case 9: // GetDeviceDontPropagateList: window
		if (!xserver_window(xserver, xin32(in, 4))) {
			xs_error(client, XS_ERROR_WINDOW, xin32(in, 4), XS_XI_OPCODE, minor);
			return true;
		}
		xs_reply_begin(client, &out, minor);
		xout_zero(&out, 24);
		xs_reply_send(client, &out);
		return true;
	case 10: // GetDeviceMotionEvents: start, stop, device. no history
		if (xi1_device(client, xin8(in, 12), minor, POINTERS)) {
			xs_reply_begin(client, &out, minor);
			xout32(&out, 0);
			xout8(&out, VALUATORS);
			xout8(&out, 0); // relative
			xout_zero(&out, 18);
			xs_reply_send(client, &out);
		}
		return true;
	case 11: // ChangeKeyboardDevice
	case 12: // ChangePointerDevice: Xorg turns them down since XI2
		xs_error(client, BAD_DEVICE, xin8(in, 4), XS_XI_OPCODE, minor);
		return true;
	case 13: // GrabDevice: window, time, classes, modes, owner events, device
		if (xi1_device(client, xin8(in, 17), minor, ANY_DEVICE)) {
			xi1_status(client, minor, 0); // Success
		}
		return true;
	case 20: // GetDeviceFocus: device. the keyboard's is the core focus
		if (xi1_device(client, xin8(in, 4), minor, KEYBOARDS)) {
			xs_reply_begin(client, &out, minor);
			xout32(&out, xserver->focus);
			xout32(&out, 0); // time
			xout8(&out, 1); // RevertToPointerRoot
			xout_zero(&out, 15);
			xs_reply_send(client, &out);
		}
		return true;
	case 22: // GetFeedbackControl: device. no feedbacks
		if (xi1_device(client, xin8(in, 4), minor, ANY_DEVICE)) {
			xs_reply_begin(client, &out, minor);
			xout16(&out, 0);
			xout_zero(&out, 22);
			xs_reply_send(client, &out);
		}
		return true;
	case 24: // GetDeviceKeyMapping: device, first, count. the core keymap
		if (xi1_device(client, xin8(in, 4), minor, KEYBOARDS)) {
			uint8_t first = xin8(in, 5), count = xin8(in, 6);
			if (first < 8 || first + count - 1 > 255) {
				xs_error(client, XS_ERROR_VALUE, first, XS_XI_OPCODE, minor);
				return true;
			}
			xs_keyboard_mapping_reply(client, first, count, minor);
		}
		return true;
	case 26: // GetDeviceModifierMapping: device
		if (xi1_device(client, xin8(in, 4), minor, KEYBOARDS)) {
			xs_modifier_mapping_reply(client, minor);
		}
		return true;
	case 27: // SetDeviceModifierMapping: MappingSuccess, nothing changes
		if (xi1_device(client, xin8(in, 4), minor, KEYBOARDS)) {
			xi1_status(client, minor, 0);
		}
		return true;
	case 28: // GetDeviceButtonMapping: device. every button itself
		if (xi1_device(client, xin8(in, 4), minor, POINTERS)) {
			xs_reply_begin(client, &out, minor);
			xout8(&out, BUTTONS);
			xout_zero(&out, 23);
			for (uint8_t b = 1; b <= BUTTONS; b++) {
				xout8(&out, b);
			}
			xout_pad(&out);
			xs_reply_send(client, &out);
		}
		return true;
	case 29: // SetDeviceButtonMapping: MappingSuccess, nothing changes
	case 33: // SetDeviceValuators: Success, nothing changes
		if (xi1_device(client, xin8(in, 4), minor, POINTERS)) {
			xi1_status(client, minor, 0);
		}
		return true;
	case 30: // QueryDeviceState: device
		if (xi1_device(client, xin8(in, 4), minor, ANY_DEVICE)) {
			query_device_state(client, xin8(in, 4));
		}
		return true;
	case 34:
		get_device_control(client, in);
		return true;
	case 35: // ChangeDeviceControl: control, device. Success, nothing changes
		if (xi1_device(client, xin8(in, 6), minor, ANY_DEVICE)) {
			xi1_status(client, minor, 0);
		}
		return true;
	// no reply, nothing changes: XI1's event selection and propagation,
	// ungrabs, passive grabs, allowing events, focus, feedback, key
	// mapping, sending events, the bell
	case 6: case 8: case 14: case 15: case 16: case 17: case 18: case 19: case 21: case 23:
	case 25: case 31: case 32:
		return true;
	default:
		return false;
	}
}

void xs_xi_request(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	struct xout out;
	uint8_t minor = xin8(in, 1);
	if (xi1_request(client, in, minor)) {
		return;
	}
	switch (minor) {
	case GET_EXTENSION_VERSION:
		get_extension_version(client);
		break;
	case 2:
		list_input_devices(client);
		break;
	case 3: { // OpenDevice: the slaves, like Xorg (masters are XI2's)
		uint8_t id = xin8(in, 4);
		if (id != MASTER_POINTER + 2 && id != MASTER_POINTER + 3) {
			xs_error(client, BAD_DEVICE, id, XS_XI_OPCODE, minor);
			break;
		}
		// class, and where its events start: XI1's DeviceKeyPress is 1,
		// DeviceButtonPress 3, DeviceValuator 0, DeviceFocusIn 6
		static const uint8_t pointer[][2] = {{1, 3}, {2, 0}, {6, 8}};
		static const uint8_t keyboard[][2] = {{0, 1}, {5, 6}, {6, 8}};
		const uint8_t (*classes)[2] = id == MASTER_POINTER + 2 ? pointer : keyboard;
		xs_reply_begin(client, &out, 3); // XI1 replies carry their request
		xout8(&out, 3);
		xout_zero(&out, 23);
		for (int i = 0; i < 3; i++) {
			xout8(&out, classes[i][0]);
			xout8(&out, (uint8_t)(XS_XI_FIRST_EVENT + classes[i][1]));
		}
		xout_zero(&out, 2);
		xs_reply_send(client, &out);
		break;
	}
	case 4: // CloseDevice
		break;
	case 40:
		query_pointer(client, in);
		break;
	case 41:
		warp_pointer(client, in);
		break;
	case 42: { // XIChangeCursor: window, cursor, device. one pointer: the window's
		struct xs_window *window = xserver_window(xserver, xin32(in, 4));
		if (!window) {
			xs_error(client, XS_ERROR_WINDOW, xin32(in, 4), XS_XI_OPCODE, minor);
			break;
		}
		xs_cursor_set(client, &window->cursor, xin32(in, 8), XS_XI_OPCODE, minor);
		break;
	}
	case 45: // XIGetClientPointer
		xs_reply_begin(client, &out, 0);
		xout8(&out, 1); // set
		xout8(&out, 0);
		xout16(&out, MASTER_POINTER);
		xs_reply_send(client, &out);
		break;
	case 46:
		select_events(client, in);
		break;
	case 47:
		query_version(client, in);
		break;
	case 48:
		query_device(client, in);
		break;
	case 49: { // XISetFocus, the keyboard's focus is the core focus
		uint32_t focus = xin32(in, 4);
		if (focus > XS_FOCUS_POINTER_ROOT && !xserver_window(xserver, focus)) {
			xs_error(client, XS_ERROR_WINDOW, focus, XS_XI_OPCODE, minor);
			break;
		}
		xs_focus_set(xserver, focus, 0);
		if (xserver->hooks.focus_changed) {
			xserver->hooks.focus_changed(xserver->hooks.data, focus);
		}
		break;
	}
	case 50: // XIGetFocus
		xs_reply_begin(client, &out, 0);
		xout32(&out, xserver->focus);
		xs_reply_send(client, &out);
		break;
	case 51: { // XIGrabDevice: a master, the pointer or the keyboard
		// grab window, time, cursor, device, modes, owner events, mask
		uint16_t device = xin16(in, 16);
		uint16_t mask_len = xin16(in, 22);
		uint32_t mask = mask_len && in->len >= 28 ? xin32(in, 24) : 0;
		uint8_t status = 0;
		if (!device_exists(device)) {
			xs_error(client, BAD_DEVICE, device, XS_XI_OPCODE, minor);
			break;
		}
		bool keyboard = device == MASTER_KEYBOARD || device == SLAVE_KEYBOARD;
		uint32_t cursor_id = keyboard ? 0 : xin32(in, 12);
		struct xs_cursor *cursor = cursor_id ? xs_cursor_find(xserver, cursor_id) : NULL;
		if (cursor_id && !cursor) {
			xs_error(client, XS_ERROR_CURSOR, cursor_id, XS_XI_OPCODE, minor);
			break;
		}
		status = xs_grab(xserver, client, keyboard, xin32(in, 4), xin8(in, 20), true, mask, 0);
		if (status == 0 && !keyboard) {
			xs_grab_cursor(xserver, cursor);
		}
		xs_reply_begin(client, &out, 0);
		xout8(&out, status);
		xs_reply_send(client, &out);
		break;
	}
	case 52: { // XIUngrabDevice: time, device
		uint16_t device = xin16(in, 8);
		xs_ungrab(xserver, client, device == MASTER_KEYBOARD || device == SLAVE_KEYBOARD);
		break;
	}
	case 54: // XIPassiveGrabDevice: none of the modifiers failed
		xs_reply_begin(client, &out, 0);
		xout16(&out, 0);
		xs_reply_send(client, &out);
		break;
	case 36: { // ListDeviceProperties: none
		uint8_t id = xin8(in, 4);
		if (!device_exists(id)) {
			xs_error(client, BAD_DEVICE, id, XS_XI_OPCODE, minor);
			break;
		}
		xs_reply_begin(client, &out, minor);
		xout16(&out, 0);
		xs_reply_send(client, &out);
		break;
	}
	case 39: { // GetDeviceProperty: there's no such property, Xorg's None
		uint8_t id = xin8(in, 20);
		if (!device_exists(id)) {
			xs_error(client, BAD_DEVICE, id, XS_XI_OPCODE, minor);
			break;
		}
		xs_reply_begin(client, &out, minor);
		xout32(&out, 0); // type None
		xout32(&out, 0); // bytes after
		xout32(&out, 0); // items
		xout8(&out, 0); // format
		xout8(&out, id);
		xs_reply_send(client, &out);
		break;
	}
	case 56: // XIListProperties: none
		xs_reply_begin(client, &out, 0);
		xout16(&out, 0);
		xs_reply_send(client, &out);
		break;
	case 59:
		get_property(client);
		break;
	case 60:
		get_selected_events(client, in);
		break;
	// accepted, nothing changes: the hierarchy, the client pointer,
	// ungrabs, allowing events, device properties, barriers
	case 37: case 38: case 43: case 44: case 53: case 55:
	case 57: case 58:
	case 61:
		break;
	default:
		xs_error(client, XS_ERROR_REQUEST, 0, XS_XI_OPCODE, minor);
		break;
	}
}

void xs_xi_client_gone(struct flame_xserver *xserver, struct xs_client *client) {
	struct select *select, *tmp;
	wl_list_for_each_safe(select, tmp, &xserver->xi_selects, link) {
		if (!client || select->client == client) {
			wl_list_remove(&select->link);
			free(select);
		}
	}
}

void xs_xi_window_gone(struct flame_xserver *xserver, uint32_t window) {
	struct select *select, *tmp;
	wl_list_for_each_safe(select, tmp, &xserver->xi_selects, link) {
		if (select->window == window) {
			wl_list_remove(&select->link);
			free(select);
		}
	}
}
