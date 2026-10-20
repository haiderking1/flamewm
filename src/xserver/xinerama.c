#include "xserver/internal.h"

// XINERAMA, the old way to ask where the monitors are: java, wine and older
// toolkits still do. the same outputs RandR tells about

void xs_xinerama_request(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	struct xout out;
	size_t len;
	const struct xs_monitor *all = xs_monitors(xserver, &len);
	uint8_t minor = xin8(in, 1);
	switch (minor) {
	case 0: // QueryVersion: 1.1
		xs_reply_begin(client, &out, 0);
		xout16(&out, 1);
		xout16(&out, 1);
		xs_reply_send(client, &out);
		break;
	case 1: // GetState: on
		xs_reply_begin(client, &out, 1);
		xout32(&out, xin32(in, 4));
		xs_reply_send(client, &out);
		break;
	case 2: // GetScreenCount
		xs_reply_begin(client, &out, (uint8_t)len);
		xout32(&out, xin32(in, 4));
		xs_reply_send(client, &out);
		break;
	case 3: { // GetScreenSize
		uint32_t screen = xin32(in, 8);
		const struct xs_monitor *m = screen < len ? &all[screen] : NULL;
		xs_reply_begin(client, &out, 0);
		xout32(&out, m ? (uint32_t)m->width : 0);
		xout32(&out, m ? (uint32_t)m->height : 0);
		xout32(&out, xin32(in, 4));
		xout32(&out, screen);
		xs_reply_send(client, &out);
		break;
	}
	case 4: // IsActive
		xs_reply_begin(client, &out, 0);
		xout32(&out, 1);
		xs_reply_send(client, &out);
		break;
	case 5: // QueryScreens, the primary first like Xorg has it
		xs_reply_begin(client, &out, 0);
		xout32(&out, (uint32_t)len);
		xout_zero(&out, 20);
		for (int pass = 0; pass < 2; pass++) {
			for (size_t i = 0; i < len; i++) {
				if (all[i].primary != (pass == 0)) {
					continue;
				}
				xout16(&out, (uint16_t)all[i].x);
				xout16(&out, (uint16_t)all[i].y);
				xout16(&out, (uint16_t)all[i].width);
				xout16(&out, (uint16_t)all[i].height);
			}
		}
		xs_reply_send(client, &out);
		break;
	default:
		xs_error(client, XS_ERROR_REQUEST, 0, XS_XINERAMA_OPCODE, minor);
		break;
	}
}
