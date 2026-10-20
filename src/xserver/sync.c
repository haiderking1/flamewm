#include <stdlib.h>
#include <X11/xshmfence.h>

#include "xserver/internal.h"

// SYNC, for its fences: DRI3 hands them over as xshmfences, Present triggers
// one when it's done with a pixmap. counters and alarms are accepted and
// forgotten, nobody here waits on them

static struct xs_fence *find_fence(struct flame_xserver *xserver, uint32_t id) {
	struct xs_fence *fence;
	wl_list_for_each(fence, &xserver->fences, link) {
		if (fence->id == id) {
			return fence;
		}
	}
	return NULL;
}

struct xs_fence *xs_fence_create(struct xs_client *client, uint32_t id, struct xshmfence *shm,
		bool triggered) {
	struct xs_fence *fence = calloc(1, sizeof(*fence));
	if (!fence) {
		return NULL;
	}
	fence->id = id;
	fence->owner = client;
	fence->shm = shm;
	fence->triggered = triggered;
	if (shm && triggered) {
		xshmfence_trigger(shm);
	}
	wl_list_insert(&client->xserver->fences, &fence->link);
	return fence;
}

void xs_fence_destroy(struct xs_fence *fence) {
	wl_list_remove(&fence->link);
	if (fence->shm) {
		xshmfence_unmap_shm(fence->shm);
	}
	free(fence);
}

void xs_fence_trigger(struct flame_xserver *xserver, uint32_t id) {
	struct xs_fence *fence = find_fence(xserver, id);
	if (!fence) {
		return;
	}
	fence->triggered = true;
	if (fence->shm) {
		xshmfence_trigger(fence->shm);
	}
}

void xs_sync_request(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	struct xout out;
	struct xs_fence *fence;
	uint8_t minor = xin8(in, 1);
	switch (minor) {
	case 0: // Initialize: 3.1 has fences
		xs_reply_begin(client, &out, 0);
		xout8(&out, 3);
		xout8(&out, 1);
		xs_reply_send(client, &out);
		break;
	case 1: // ListSystemCounters: none
		xs_reply_begin(client, &out, 0);
		xout32(&out, 0);
		xs_reply_send(client, &out);
		break;
	case 5: // QueryCounter: they all stay at zero
		xs_reply_begin(client, &out, 0);
		xout32(&out, 0);
		xout32(&out, 0);
		xs_reply_send(client, &out);
		break;
	case 10: // QueryAlarm
		xs_reply_begin(client, &out, 0);
		xout_zero(&out, 32);
		xs_reply_send(client, &out);
		break;
	case 13: // GetPriority
		xs_reply_begin(client, &out, 0);
		xout32(&out, 0);
		xs_reply_send(client, &out);
		break;
	case 14: // CreateFence
		if (!xs_fence_create(client, xin32(in, 8), NULL, xin8(in, 12))) {
			xs_error(client, XS_ERROR_ALLOC, 0, XS_SYNC_OPCODE, minor);
		}
		break;
	case 15: // TriggerFence
		xs_fence_trigger(xserver, xin32(in, 4));
		break;
	case 16: // ResetFence
		if ((fence = find_fence(xserver, xin32(in, 4)))) {
			fence->triggered = false;
			if (fence->shm) {
				xshmfence_reset(fence->shm);
			}
		}
		break;
	case 17: // DestroyFence
		if ((fence = find_fence(xserver, xin32(in, 4)))) {
			xs_fence_destroy(fence);
		}
		break;
	case 18: // QueryFence
		fence = find_fence(xserver, xin32(in, 4));
		xs_reply_begin(client, &out, 0);
		xout8(&out, fence ? (fence->shm ? xshmfence_query(fence->shm) : fence->triggered) : 1);
		xs_reply_send(client, &out);
		break;
	// counters, alarms, priorities, awaits: accepted, nothing waits here
	case 2: case 3: case 4: case 6: case 7: case 8: case 9: case 11: case 12: case 19:
		break;
	default:
		xs_error(client, XS_ERROR_REQUEST, 0, XS_SYNC_OPCODE, minor);
		break;
	}
}
