#include "xserver/internal.h"

// X-Resource, for one question: which process does this window belong to.
// steam's main process asks it about its UI's windows and gives up on the
// UI when there's no answer. pids come from the socket's credentials, the
// same as Xorg does it

#define CLIENT_XID_MASK 1
#define CLIENT_PID_MASK 2

static struct xs_client *owner_of(struct flame_xserver *xserver, uint32_t xid) {
	struct xs_client *client;
	wl_list_for_each(client, &xserver->clients, link) {
		if ((xid & ~0x1fffffu) == client->id_base) {
			return client;
		}
	}
	return NULL;
}

// one ClientIdValue: the spec it answers, then its value
static void put_id(struct xout *out, struct xs_client *client, uint32_t mask) {
	xout32(out, client->id_base);
	xout32(out, mask);
	if (mask == CLIENT_PID_MASK) {
		xout32(out, 4);
		xout32(out, (uint32_t)client->pid);
	} else {
		xout32(out, 0);
	}
}

static void query_client_ids(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	uint32_t specs = xin32(in, 4);
	if (8 + (size_t)specs * 8 > in->len) {
		xs_error(client, XS_ERROR_LENGTH, 0, XS_XRES_OPCODE, 4);
		return;
	}
	struct xout out;
	xs_reply_begin(client, &out, 0);
	size_t count_at = out.buf.len;
	xout32(&out, 0); // filled in below
	xout_zero(&out, 20);
	uint32_t count = 0;
	for (uint32_t i = 0; i < specs; i++) {
		uint32_t xid = xin32(in, 8 + i * 8), mask = xin32(in, 12 + i * 8);
		// no xid: every client
		struct xs_client *each;
		wl_list_for_each(each, &xserver->clients, link) {
			if (xid && each != owner_of(xserver, xid)) {
				continue;
			}
			for (uint32_t bit = CLIENT_XID_MASK; bit <= CLIENT_PID_MASK; bit <<= 1) {
				if (!mask || (mask & bit)) {
					put_id(&out, each, bit);
					count++;
				}
			}
		}
	}
	if (!out.buf.failed) {
		memcpy(out.buf.data + count_at, &count, 4);
	}
	xs_reply_send(client, &out);
}

void xs_xres_request(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	struct xout out;
	uint8_t minor = xin8(in, 1);
	switch (minor) {
	case 0: // QueryVersion: 1.2 has QueryClientIds
		xs_reply_begin(client, &out, 0);
		xout16(&out, 1);
		xout16(&out, 2);
		xs_reply_send(client, &out);
		break;
	case 1: { // QueryClients
		xs_reply_begin(client, &out, 0);
		xout32(&out, (uint32_t)wl_list_length(&xserver->clients));
		xout_zero(&out, 20);
		struct xs_client *each;
		wl_list_for_each(each, &xserver->clients, link) {
			xout32(&out, each->id_base);
			xout32(&out, 0x1fffff);
		}
		xs_reply_send(client, &out);
		break;
	}
	case 2: // QueryClientResources: nobody's counting
	case 5: // QueryResourceBytes
		xs_reply_begin(client, &out, 0);
		xout32(&out, 0);
		xs_reply_send(client, &out);
		break;
	case 3: // QueryClientPixmapBytes: nothing is drawn, nothing is kept
		xs_reply_begin(client, &out, 0);
		xout32(&out, 0);
		xout32(&out, 0);
		xs_reply_send(client, &out);
		break;
	case 4:
		query_client_ids(client, in);
		break;
	default:
		xs_error(client, XS_ERROR_REQUEST, 0, XS_XRES_OPCODE, minor);
		break;
	}
}
