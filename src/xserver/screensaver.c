#include "xserver/internal.h"

// MIT-SCREEN-SAVER and DPMS: there's no X screen saver, flame owns the
// screen. what apps get out of them is how long since the last input (chat
// apps go "away" on it) and a yes to video players keeping the screen on

void xs_screensaver_suspend(struct xs_client *client, bool suspend) {
	struct flame_xserver *xserver = client->xserver;
	int was = xserver->screensaver_suspends;
	if (suspend) {
		client->screensaver_suspends++;
		xserver->screensaver_suspends++;
	} else if (client->screensaver_suspends > 0) {
		client->screensaver_suspends--;
		xserver->screensaver_suspends--;
	}
	if ((was > 0) != (xserver->screensaver_suspends > 0) && xserver->hooks.screensaver) {
		xserver->hooks.screensaver(xserver->hooks.data, xserver->screensaver_suspends > 0);
	}
}

void xs_screensaver_request(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	struct xout out;
	uint8_t minor = xin8(in, 1);
	switch (minor) {
	case 0: // QueryVersion: 1.1
		xs_reply_begin(client, &out, 0);
		xout16(&out, 1);
		xout16(&out, 1);
		xs_reply_send(client, &out);
		break;
	case 1: // QueryInfo
		xs_reply_begin(client, &out, 3); // disabled
		xout32(&out, 0); // no saver window
		xout32(&out, 0); // till it comes on
		xout32(&out, xs_now(xserver) - xserver->last_input);
		xout32(&out, 0); // event mask
		xout8(&out, 0); // kind: blanked
		xs_reply_send(client, &out);
		break;
	// SelectInput, SetAttributes, UnsetAttributes: there's no saver of X's
	case 2: case 3: case 4:
		break;
	case 5: // Suspend: true and false nest, flame keeps the screen on meanwhile
		xs_screensaver_suspend(client, xin32(in, 4) != 0);
		break;
	default:
		xs_error(client, XS_ERROR_REQUEST, 0, XS_SCREENSAVER_OPCODE, minor);
		break;
	}
}

void xs_dpms_request(struct xs_client *client, const struct xin *in) {
	struct xout out;
	uint8_t minor = xin8(in, 1);
	switch (minor) {
	case 0: // GetVersion: 1.1
		xs_reply_begin(client, &out, 0);
		xout16(&out, 1);
		xout16(&out, 1);
		xs_reply_send(client, &out);
		break;
	case 1: // Capable: no, flame does the outputs' power
		xs_reply_begin(client, &out, 0);
		xout8(&out, 0);
		xs_reply_send(client, &out);
		break;
	case 2: // GetTimeouts: none
		xs_reply_begin(client, &out, 0);
		xout_zero(&out, 6);
		xs_reply_send(client, &out);
		break;
	case 7: // Info: on, DPMS off
		xs_reply_begin(client, &out, 0);
		xout16(&out, 0);
		xout8(&out, 0);
		xs_reply_send(client, &out);
		break;
	case 3: case 4: case 5: case 6: // SetTimeouts, Enable, Disable, ForceLevel
		break;
	default:
		xs_error(client, XS_ERROR_REQUEST, 0, XS_DPMS_OPCODE, minor);
		break;
	}
}
