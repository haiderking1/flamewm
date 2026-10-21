#include <string.h>
#include <sys/socket.h>

#include "xserver/internal.h"

// input for X windows: flame decides which top-level the pointer is over and
// who has the keyboard, this turns that into X events. events start at the
// deepest window and climb until someone asked for them, like X does

#define EVENT_KEY_PRESS 2
#define EVENT_KEY_RELEASE 3
#define EVENT_BUTTON_PRESS 4
#define EVENT_BUTTON_RELEASE 5
#define EVENT_MOTION 6
#define EVENT_ENTER 7
#define EVENT_LEAVE 8
#define EVENT_CLIENT_MESSAGE 33

#define MASK_KEY_PRESS (1u << 0)
#define MASK_KEY_RELEASE (1u << 1)
#define MASK_BUTTON_PRESS (1u << 2)
#define MASK_BUTTON_RELEASE (1u << 3)
#define MASK_ENTER (1u << 4)
#define MASK_LEAVE (1u << 5)
#define MASK_POINTER_MOTION (1u << 6)
#define MASK_BUTTON1_MOTION (1u << 8)
#define MASK_BUTTON_MOTION (1u << 13)

#define STATE_BUTTONS 0x1f00

#define SCROLL_NOTCH 15 // a wheel notch, in wayland's axis units

// the deepest viewable window under a root point, starting at top
static struct xs_window *deepest(struct xs_window *top, int x, int y) {
	struct xs_window *window = top;
	struct xs_window *child;
	while ((child = xs_window_child_at(window, x, y))) {
		window = child;
	}
	return window;
}

// fills the fields every pointer and key event shares, for receiver
static void fill(struct flame_xserver *xserver, struct xs_event *event, uint8_t type,
		uint8_t detail, struct xs_window *receiver, struct xs_window *source, uint16_t state,
		uint32_t time) {
	memset(event, 0, sizeof(*event));
	int rx = xserver->pointer_x, ry = xserver->pointer_y;
	int ox, oy;
	xs_window_origin(receiver, &ox, &oy);
	// the child of receiver the event came through, if any
	uint32_t child = 0;
	for (struct xs_window *w = source; w && w != receiver; w = w->parent) {
		if (w->parent == receiver) {
			child = w->id;
		}
	}
	xs_event_put8(event, 0, type);
	xs_event_put8(event, 1, detail);
	xs_event_put32(event, 4, time);
	xs_event_put32(event, 8, XS_ROOT);
	xs_event_put32(event, 12, receiver->id);
	xs_event_put32(event, 16, child);
	xs_event_put16(event, 20, (uint16_t)rx);
	xs_event_put16(event, 22, (uint16_t)ry);
	xs_event_put16(event, 24, (uint16_t)(rx - ox));
	xs_event_put16(event, 26, (uint16_t)(ry - oy));
	xs_event_put16(event, 28, state);
	xs_event_put8(event, 30, 1); // same screen
}

// from source up to the first window someone listens on, core events for
// core_mask or the XI2 event, the way device events propagate. returns who
// got it, NULL for nobody
static struct xs_window *propagate(struct flame_xserver *xserver, struct xs_window *source,
		uint32_t core_mask, uint8_t type, const struct xs_input *input) {
	for (struct xs_window *w = source; w; w = w->parent) {
		bool xi2 = !input->core_only && xs_xi_selected(xserver, w, input->evtype);
		bool core = core_mask && xs_event_wanted_input(w, core_mask, input->evtype);
		if (xi2 || core) {
			if (xi2) {
				xs_xi_device_event(xserver, w, source, input);
			}
			if (core) {
				struct xs_event event;
				fill(xserver, &event, type, (uint8_t)input->detail, w, source, input->state,
					input->time);
				xs_event_deliver_input(xserver, w, core_mask, input->evtype, &event,
					input->core_only);
			}
			return w;
		}
		if (w->do_not_propagate & core_mask) {
			return NULL;
		}
	}
	return NULL;
}

#define NOTIFY_ANCESTOR 0
#define NOTIFY_VIRTUAL 1
#define NOTIFY_INFERIOR 2
#define NOTIFY_NONLINEAR 3
#define NOTIFY_NONLINEAR_VIRTUAL 4

// one EnterNotify or LeaveNotify. child: the window inside window on the
// pointer's way, NULL for none
static void crossing(struct flame_xserver *xserver, uint8_t type, struct xs_window *window,
		uint8_t detail, struct xs_window *child, uint16_t state, uint32_t time) {
	struct xs_event event;
	fill(xserver, &event, type, detail, window, child, state, time);
	xs_event_put8(&event, 30, 0); // mode: normal
	// same screen, and focus: the keyboard's on this window or one it's in
	xs_event_put8(&event, 31, 2 | (xs_focus_within(xserver, window) ? 1 : 0));
	bool enter = type == EVENT_ENTER;
	int evtype = enter ? XS_XI_ENTER : XS_XI_LEAVE;
	xs_event_deliver_input(xserver, window, enter ? MASK_ENTER : MASK_LEAVE, evtype, &event,
		false);
	xs_xi_crossing(xserver, evtype, window, detail, child, state, time);
}

// LeaveNotify up from below from (not included) to stop (not included),
// each with the window it came out of
static void leave_up(struct flame_xserver *xserver, struct xs_window *from,
		struct xs_window *stop, uint8_t detail, uint16_t state, uint32_t time) {
	struct xs_window *child = from;
	for (struct xs_window *w = from->parent; w && w != stop; w = w->parent) {
		crossing(xserver, EVENT_LEAVE, w, detail, child, state, time);
		child = w;
	}
}

// EnterNotify down from below stop to the parent of to, each with the
// window it goes on into
static void enter_down(struct flame_xserver *xserver, struct xs_window *stop,
		struct xs_window *to, uint8_t detail, uint16_t state, uint32_t time) {
	struct xs_window *chain[64];
	int len = 0;
	for (struct xs_window *w = to; w->parent && w->parent != stop && len < 64; w = w->parent) {
		chain[len++] = w;
	}
	while (len > 0) {
		struct xs_window *child = chain[--len];
		crossing(xserver, EVENT_ENTER, child->parent, detail, child, state, time);
	}
}

// the pointer went from a to b (NULL: out of X's windows, onto the root):
// the protocol's crossing events, Xorg's way. a toolkit whose window the
// pointer goes into a child of hears NotifyInferior, not that it left
static void cross(struct flame_xserver *xserver, struct xs_window *a, struct xs_window *b,
		uint16_t state, uint32_t time) {
	a = a ? a : xserver->root;
	b = b ? b : xserver->root;
	if (a == b) {
		return;
	}
	if (xs_window_inferior(a, b)) {
		crossing(xserver, EVENT_LEAVE, a, NOTIFY_ANCESTOR, NULL, state, time);
		leave_up(xserver, a, b, NOTIFY_VIRTUAL, state, time);
		crossing(xserver, EVENT_ENTER, b, NOTIFY_INFERIOR, NULL, state, time);
	} else if (xs_window_inferior(b, a)) {
		crossing(xserver, EVENT_LEAVE, a, NOTIFY_INFERIOR, NULL, state, time);
		enter_down(xserver, a, b, NOTIFY_VIRTUAL, state, time);
		crossing(xserver, EVENT_ENTER, b, NOTIFY_ANCESTOR, NULL, state, time);
	} else {
		struct xs_window *common = xs_window_common(a, b);
		crossing(xserver, EVENT_LEAVE, a, NOTIFY_NONLINEAR, NULL, state, time);
		leave_up(xserver, a, common, NOTIFY_NONLINEAR_VIRTUAL, state, time);
		enter_down(xserver, common, b, NOTIFY_NONLINEAR_VIRTUAL, state, time);
		crossing(xserver, EVENT_ENTER, b, NOTIFY_NONLINEAR, NULL, state, time);
	}
}

static bool alive(struct flame_xserver *xserver, uint32_t id) {
	return id && xserver_window(xserver, id);
}

void xserver_pointer_motion(struct flame_xserver *xserver, struct xs_window *top, int x,
		int y, uint16_t state, uint32_t time) {
	xserver->pointer_x = x;
	xserver->pointer_y = y;
	xserver->state = state;
	// a held button keeps everything going where it went down
	struct xs_window *grab = alive(xserver, xserver->grab) && (state & STATE_BUTTONS)
		? xserver_window(xserver, xserver->grab) : NULL;
	struct xs_window *under = grab ? NULL : top ? deepest(top, x, y) : NULL;

	struct xs_window *previous = alive(xserver, xserver->pointer_window)
		? xserver_window(xserver, xserver->pointer_window) : NULL;
	if (!grab && under != previous) {
		// where it is first: focus flags and anything asked back see it
		xserver->pointer_window = under ? under->id : 0;
		cross(xserver, previous, under, state, time);
		xserver->scroll_primed = false; // toolkits forget the valuators on enter
	}

	struct xs_window *source = grab ? grab : under;
	if (!source) {
		return;
	}
	uint32_t mask = MASK_POINTER_MOTION;
	if (state & STATE_BUTTONS) {
		mask |= MASK_BUTTON_MOTION;
		for (int b = 0; b < 5; b++) {
			if (state & (1u << (8 + b))) {
				mask |= MASK_BUTTON1_MOTION << b;
			}
		}
	}
	struct xs_input input = {.evtype = XS_XI_MOTION, .time = time, .state = state};
	propagate(xserver, source, mask, EVENT_MOTION, &input);
}

void xserver_pointer_raw_motion(struct flame_xserver *xserver, double dx, double dy,
		double raw_dx, double raw_dy, uint32_t time) {
	xs_xi_raw(xserver, XS_XI_RAW_MOTION, 0, dx, dy, raw_dx, raw_dy, time);
}

// where button events go: the grab, else what's under the pointer
static struct xs_window *button_source(struct flame_xserver *xserver) {
	if (alive(xserver, xserver->grab)) {
		return xserver_window(xserver, xserver->grab);
	}
	if (alive(xserver, xserver->pointer_window)) {
		return xserver_window(xserver, xserver->pointer_window);
	}
	return NULL;
}

static void button(struct flame_xserver *xserver, uint8_t button, bool pressed,
		uint16_t state, uint32_t time, uint32_t flags) {
	struct xs_window *source = button_source(xserver);
	if (!source) {
		return;
	}
	struct xs_input input = {
		.evtype = pressed ? XS_XI_BUTTON_PRESS : XS_XI_BUTTON_RELEASE,
		.detail = button,
		.time = time,
		.state = state,
		.flags = flags,
	};
	struct xs_window *got = propagate(xserver, source,
		pressed ? MASK_BUTTON_PRESS : MASK_BUTTON_RELEASE,
		pressed ? EVENT_BUTTON_PRESS : EVENT_BUTTON_RELEASE, &input);
	if (pressed && got && !xserver->grab) {
		xserver->grab = got->id; // the implicit grab, until the last button lets go
	}
	uint16_t held = state & STATE_BUTTONS & (uint16_t)~(1u << (7 + button));
	if (!pressed && !held) {
		xserver->grab = 0;
	}
}

void xserver_pointer_button(struct flame_xserver *xserver, uint8_t number, bool pressed,
		uint16_t state, uint32_t time) {
	xserver->state = state;
	xs_xi_raw(xserver, pressed ? XS_XI_RAW_BUTTON_PRESS : XS_XI_RAW_BUTTON_RELEASE, number,
		0, 0, 0, 0, time);
	button(xserver, number, pressed, state, time, 0);
}

void xserver_pointer_scroll(struct flame_xserver *xserver, bool vertical, double delta,
		uint16_t state, uint32_t time) {
	xserver->state = state;
	struct xs_window *source = button_source(xserver);
	if (!source) {
		return;
	}
	int axis = vertical ? 0 : 1;
	// XI2: the scroll valuator moves, toolkits scroll smoothly by how much.
	// they only count changes, so the first scroll after the pointer came in
	// starts with the valuators as they are
	struct xs_input input = {
		.evtype = XS_XI_MOTION,
		.time = time,
		.state = state,
		.scroll = 3,
	};
	if (!xserver->scroll_primed) {
		propagate(xserver, source, 0, EVENT_MOTION, &input);
		xserver->scroll_primed = true;
	}
	xserver->scroll[axis] += delta;
	input.scroll = vertical ? 1 : 2;
	propagate(xserver, source, 0, EVENT_MOTION, &input);
	// everyone else: buttons 4 to 7, a click per wheel notch. touchpads send
	// small steps, they add up. XI2 clients get them marked as emulated
	double *notches = &xserver->scroll_notches[axis];
	*notches += delta;
	while (*notches <= -SCROLL_NOTCH || *notches >= SCROLL_NOTCH) {
		bool back = *notches < 0;
		uint8_t number = vertical ? (back ? 4 : 5) : (back ? 6 : 7);
		button(xserver, number, true, state, time, XS_XI_POINTER_EMULATED);
		button(xserver, number, false, state, time, XS_XI_POINTER_EMULATED);
		*notches += back ? SCROLL_NOTCH : -SCROLL_NOTCH;
	}
}

void xserver_pointer_leave(struct flame_xserver *xserver, uint32_t time) {
	struct xs_window *previous = alive(xserver, xserver->pointer_window)
		? xserver_window(xserver, xserver->pointer_window) : NULL;
	xserver->pointer_window = 0;
	if (previous) {
		cross(xserver, previous, NULL, 0, time);
	}
}

static void key(struct flame_xserver *xserver, uint8_t keycode, bool pressed, uint16_t state,
		uint32_t time, uint32_t flags, bool core_only) {
	struct xs_window *under = alive(xserver, xserver->pointer_window)
		? xserver_window(xserver, xserver->pointer_window) : NULL;
	// PointerRoot: keys follow the pointer, whatever window it's over
	struct xs_window *focus = xserver->focus == XS_FOCUS_POINTER_ROOT ? xserver->root
		: alive(xserver, xserver->focus) ? xserver_window(xserver, xserver->focus) : NULL;
	if (!focus) {
		return;
	}
	// to the window under the pointer when it's inside the focus window
	struct xs_window *source = focus;
	for (struct xs_window *w = under; w; w = w->parent) {
		if (w == focus) {
			source = under;
			break;
		}
	}
	struct xs_input input = {
		.evtype = pressed ? XS_XI_KEY_PRESS : XS_XI_KEY_RELEASE,
		.detail = keycode,
		.time = time,
		.state = state,
		.flags = flags,
		.core_only = core_only,
	};
	propagate(xserver, source, pressed ? MASK_KEY_PRESS : MASK_KEY_RELEASE,
		pressed ? EVENT_KEY_PRESS : EVENT_KEY_RELEASE, &input);
}

void xserver_key(struct flame_xserver *xserver, uint8_t keycode, bool pressed, uint16_t state,
		uint32_t time) {
	xserver->state = (uint16_t)((xserver->state & STATE_BUTTONS) | (state & ~STATE_BUTTONS));
	xs_xi_raw(xserver, pressed ? XS_XI_RAW_KEY_PRESS : XS_XI_RAW_KEY_RELEASE, keycode,
		0, 0, 0, 0, time);
	key(xserver, keycode, pressed, state, time, 0, false);
}

void xserver_key_repeat(struct flame_xserver *xserver, uint8_t keycode, uint16_t state,
		uint32_t time) {
	// core clients see a release and a press, like Xorg sends them. XI2
	// clients only a press, marked as a repeat
	key(xserver, keycode, false, state, time, 0, true);
	key(xserver, keycode, true, state, time, XS_XI_KEY_REPEAT, false);
}

bool xserver_close_window(struct flame_xserver *xserver, struct xs_window *window) {
	uint32_t protocols = xs_atom_intern(&xserver->atoms, "WM_PROTOCOLS", 12, false);
	uint32_t delete = xs_atom_intern(&xserver->atoms, "WM_DELETE_WINDOW", 16, false);
	struct xs_property *property = protocols ? xs_window_property(window, protocols) : NULL;
	bool polite = false;
	if (property && property->format == 32 && delete) {
		for (uint32_t i = 0; i < property->units; i++) {
			uint32_t atom;
			memcpy(&atom, property->data + i * 4, 4);
			polite |= atom == delete;
		}
	}
	if (!polite || !window->owner) {
		// it never said it can close windows: the whole client goes, like
		// xkill. the loop reaps it when its socket hangs up
		if (window->owner) {
			shutdown(window->owner->fd, SHUT_RDWR);
		}
		return false;
	}
	struct xs_event event = {0};
	xs_event_put8(&event, 0, EVENT_CLIENT_MESSAGE);
	xs_event_put8(&event, 1, 32); // format
	xs_event_put32(&event, 4, window->id);
	xs_event_put32(&event, 8, protocols);
	xs_event_put32(&event, 12, delete);
	xs_event_put32(&event, 16, xs_now(xserver));
	xs_event_send(window->owner, &event);
	return true;
}
