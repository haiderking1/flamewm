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

#define BUTTONS 9
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
		uint8_t *event, size_t len, uint16_t master, uint16_t slave) {
	struct select *select;
	wl_list_for_each(select, &xserver->xi_selects, link) {
		if (select->window != window) {
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
	deliver(xserver, receiver->id, input->evtype, event, len,
		key ? MASTER_KEYBOARD : MASTER_POINTER, key ? SLAVE_KEYBOARD : SLAVE_POINTER);
}

// Enter, Leave, FocusIn and FocusOut share a layout
static void crossing(struct flame_xserver *xserver, int evtype, struct xs_window *window,
		uint8_t detail, uint16_t state, uint32_t time, bool focus) {
	int rx = xserver->pointer_x, ry = xserver->pointer_y;
	int ox, oy;
	xs_window_origin(window, &ox, &oy);
	uint8_t event[76];
	header(event, sizeof(event), (uint16_t)evtype, 0, time);
	put16(event + 16, focus ? SLAVE_KEYBOARD : SLAVE_POINTER);
	event[18] = 0; // mode: normal
	event[19] = detail;
	put32(event + 20, XS_ROOT);
	put32(event + 24, window->id);
	put32(event + 28, 0); // child
	put32(event + 32, fp1616(rx));
	put32(event + 36, fp1616(ry));
	put32(event + 40, fp1616(rx - ox));
	put32(event + 44, fp1616(ry - oy));
	event[48] = 1; // same screen
	event[49] = xserver->focus == window->id;
	put16(event + 50, 1); // buttons, in units
	put_mods(event + 52, state);
	put32(event + 72, buttons_of(state));
	deliver(xserver, window->id, evtype, event, sizeof(event),
		focus ? MASTER_KEYBOARD : MASTER_POINTER, focus ? SLAVE_KEYBOARD : SLAVE_POINTER);
}

void xs_xi_crossing(struct flame_xserver *xserver, int evtype, struct xs_window *window,
		uint8_t detail, uint16_t state, uint32_t time) {
	crossing(xserver, evtype, window, detail, state, time, false);
}

void xs_xi_focus(struct flame_xserver *xserver, int evtype, struct xs_window *window) {
	crossing(xserver, evtype, window, 3 /* NotifyNonlinear */, xserver->state,
		xs_now(xserver), true);
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
		key ? SLAVE_KEYBOARD : SLAVE_POINTER);
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
	int x = xserver->pointer_x, y = xserver->pointer_y;
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

static void get_property(struct xs_client *client) {
	struct xout out;
	xs_reply_begin(client, &out, 0);
	xout32(&out, 0); // type None
	xout32(&out, 0); // bytes after
	xout32(&out, 0); // items
	xout8(&out, 0); // format
	xs_reply_send(client, &out);
}

void xs_xi_request(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	struct xout out;
	uint8_t minor = xin8(in, 1);
	switch (minor) {
	case GET_EXTENSION_VERSION:
		get_extension_version(client);
		break;
	case 40:
		query_pointer(client, in);
		break;
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
	case 51: // XIGrabDevice: Success, nobody else is grabbing anything
		xs_reply_begin(client, &out, 0);
		xout8(&out, 0);
		xs_reply_send(client, &out);
		break;
	case 54: // XIPassiveGrabDevice: none of the modifiers failed
		xs_reply_begin(client, &out, 0);
		xout16(&out, 0);
		xs_reply_send(client, &out);
		break;
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
	// accepted, nothing changes: XI1's event selection, warping, cursors,
	// the hierarchy, the client pointer, ungrabs, allowing events, device
	// properties, barriers
	case 6: case 41: case 42: case 43: case 44: case 52: case 53: case 55: case 57: case 58:
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
