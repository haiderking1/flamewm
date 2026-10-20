#include <stdlib.h>

#include "xserver/internal.h"

// XFIXES, for its regions: Present takes them as arguments. nothing here
// looks inside one, flame redraws a presented window whole

static void region_create(struct xs_client *client, uint32_t id) {
	struct xs_region *region = calloc(1, sizeof(*region));
	if (!region) {
		xs_error(client, XS_ERROR_ALLOC, id, XS_XFIXES_OPCODE, 5);
		return;
	}
	region->id = id;
	region->owner = client;
	wl_list_insert(&client->xserver->regions, &region->link);
}

static void region_destroy(struct flame_xserver *xserver, uint32_t id) {
	struct xs_region *region, *tmp;
	wl_list_for_each_safe(region, tmp, &xserver->regions, link) {
		if (region->id == id) {
			wl_list_remove(&region->link);
			free(region);
		}
	}
}

void xs_xfixes_request(struct xs_client *client, const struct xin *in) {
	struct xout out;
	uint8_t minor = xin8(in, 1);
	switch (minor) {
	case 0: // QueryVersion
		xs_reply_begin(client, &out, 0);
		xout32(&out, 5);
		xout32(&out, 0);
		xs_reply_send(client, &out);
		break;
	case 4: // GetCursorImage: an empty one, flame draws the pointer
		xs_reply_begin(client, &out, 0);
		xout16(&out, 0);
		xout16(&out, 0);
		xout16(&out, 1);
		xout16(&out, 1);
		xout16(&out, 0);
		xout16(&out, 0);
		xout32(&out, 1);
		xout_zero(&out, 8);
		xout32(&out, 0);
		xs_reply_send(client, &out);
		break;
	case 5: case 6: case 7: case 8: case 9: // CreateRegion and its From* friends
		region_create(client, xin32(in, 4));
		break;
	case 10: // DestroyRegion
		region_destroy(client->xserver, xin32(in, 4));
		break;
	case 19: // FetchRegion: empty
		xs_reply_begin(client, &out, 0);
		xout_zero(&out, 8);
		xs_reply_send(client, &out);
		break;
	case 34: // GetClientDisconnectMode
		xs_reply_begin(client, &out, 0);
		xout32(&out, 0);
		xs_reply_send(client, &out);
		break;
	// region math, selections, cursors, barriers: accepted, nothing to keep
	case 2: case 3: case 11: case 12: case 13: case 14: case 15: case 16: case 17:
	case 18: case 20: case 21: case 22: case 23: case 26: case 28: case 29: case 30:
	case 31: case 32: case 33:
		break;
	default:
		xs_error(client, XS_ERROR_REQUEST, 0, XS_XFIXES_OPCODE, minor);
		break;
	}
}
