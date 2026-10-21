#include "xserver/internal.h"

// XTEST: input made up by a client (xdotool, test suites, remote desktops),
// for X windows. it goes through the same paths flame's real input does.
// the pointer flame draws doesn't move, X's idea of it does

#define STATE_BUTTONS 0x1f00

static void fake_input(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	uint8_t type = xin8(in, 4), detail = xin8(in, 5);
	uint32_t time = xs_now(xserver);
	uint16_t state = xserver->state;
	switch (type) {
	case 2: // KeyPress
	case 3: // KeyRelease
		if (detail < 8) {
			xs_error(client, XS_ERROR_VALUE, detail, XS_XTEST_OPCODE, 2);
			return;
		}
		xserver_key(xserver, detail, type == 2, state, time);
		// a modifier key counts from the next event on, like a real one:
		// xdotool's ctrl+c is a press of Control_L, then c
		uint8_t mods = xs_xkb_modmap(xserver, detail);
		if (mods) {
			const struct xs_kbd_state *kbd = &xserver->kbd;
			uint8_t base = type == 2 ? kbd->base | mods : kbd->base & (uint8_t)~mods;
			xserver->state = type == 2 ? state | mods : state & (uint16_t)~mods;
			xserver_keyboard_state(xserver, base, kbd->latched, kbd->locked, kbd->group);
		}
		break;
	case 4: // ButtonPress
	case 5: { // ButtonRelease
		if (detail < 1) {
			xs_error(client, XS_ERROR_VALUE, detail, XS_XTEST_OPCODE, 2);
			return;
		}
		uint16_t bit = detail <= 5 ? (uint16_t)(1u << (7 + detail)) : 0;
		xserver_pointer_button(xserver, detail, type == 4, state, time);
		xserver->state = type == 4 ? state | bit : state & (uint16_t)~bit;
		break;
	}
	case 6: { // MotionNotify, detail 1: relative
		int x = xin16s(in, 24), y = xin16s(in, 26);
		if (detail) {
			int px, py;
			xs_pointer_position(xserver, &px, &py);
			x += px;
			y += py;
		}
		// flame's cursor moves with it, X hears it from there
		xs_warp_pointer(xserver, NULL, x, y);
		break;
	}
	default:
		xs_error(client, XS_ERROR_VALUE, type, XS_XTEST_OPCODE, 2);
		break;
	}
}

void xs_xtest_request(struct xs_client *client, const struct xin *in) {
	struct xout out;
	uint8_t minor = xin8(in, 1);
	switch (minor) {
	case 0: // GetVersion: 2.2
		xs_reply_begin(client, &out, 2);
		xout16(&out, 2);
		xs_reply_send(client, &out);
		break;
	case 1: // CompareCursor: there's one cursor, flame's
		xs_reply_begin(client, &out, xin32(in, 8) == 0);
		xs_reply_send(client, &out);
		break;
	case 2:
		fake_input(client, in);
		break;
	case 3: // GrabControl
		break;
	default:
		xs_error(client, XS_ERROR_REQUEST, 0, XS_XTEST_OPCODE, minor);
		break;
	}
}
