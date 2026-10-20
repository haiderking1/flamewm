#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xserver/internal.h"

// RandR 1.5, read only: flame's outputs, one crtc and one mode each, the
// mode being what the output shows right now. apps take their monitor
// layout and refresh rate from it (firefox paces itself to the fastest
// crtc's mode, winit won't start without it). asking for anything else is
// accepted and changes nothing, the outputs are flame's to set

#define MAJOR 1
#define MINOR 5

#define BAD_OUTPUT (XS_RANDR_FIRST_ERROR + 0)
#define BAD_CRTC (XS_RANDR_FIRST_ERROR + 1)

#define SCREEN_CHANGE_MASK 1
#define CRTC_CHANGE_MASK 2
#define OUTPUT_CHANGE_MASK 4
#define RESOURCE_CHANGE_MASK 64

#define ROTATE_0 1
#define GAMMA_SIZE 256

// who listens on which window, RRSelectInput
struct select {
	struct wl_list link; // flame_xserver.randr_selects
	struct xs_client *client;
	uint32_t window;
	uint16_t mask;
};

// one made up from the root while flame has no outputs, so there's always one
static const struct xs_monitor *monitors(struct flame_xserver *xserver, size_t *len) {
	static struct xs_monitor fallback;
	if (xserver->monitors_len > 0) {
		*len = xserver->monitors_len;
		return xserver->monitors;
	}
	fallback = (struct xs_monitor){
		.name = "screen",
		.width = xserver->root->width,
		.height = xserver->root->height,
		.refresh = 60000,
		.primary = true,
		.id = 0x200,
	};
	*len = 1;
	return &fallback;
}

static const struct xs_monitor *find(struct flame_xserver *xserver, uint32_t id) {
	size_t len;
	const struct xs_monitor *all = monitors(xserver, &len);
	for (size_t i = 0; i < len; i++) {
		if (all[i].id == id) {
			return &all[i];
		}
	}
	return NULL;
}

// 96 dpi, the same the screen tells in the connection setup
static uint32_t mm_of(int pixels) {
	return (uint32_t)(pixels * 254 / 960);
}

static uint32_t mm_width(const struct xs_monitor *m) {
	return m->mm_width > 0 ? (uint32_t)m->mm_width : mm_of(m->width);
}

static uint32_t mm_height(const struct xs_monitor *m) {
	return m->mm_height > 0 ? (uint32_t)m->mm_height : mm_of(m->height);
}

static int refresh_of(const struct xs_monitor *m) {
	return m->refresh > 0 ? m->refresh : 60000;
}

// "1920x1080"
static size_t mode_name(const struct xs_monitor *m, char *buf, size_t size) {
	int len = snprintf(buf, size, "%dx%d", m->width, m->height);
	return len > 0 ? (size_t)len : 0;
}

// a MODEINFO with no blanking: the dot clock is what makes refresh come out
// right, dot clock / (htotal * vtotal) is how everyone computes it
static void put_mode(struct xout *out, const struct xs_monitor *m) {
	char name[32];
	uint64_t dot = (uint64_t)refresh_of(m) * (uint64_t)m->width * (uint64_t)m->height / 1000;
	xout32(out, m->id + 2);
	xout16(out, (uint16_t)m->width);
	xout16(out, (uint16_t)m->height);
	xout32(out, dot > UINT32_MAX ? UINT32_MAX : (uint32_t)dot);
	xout16(out, (uint16_t)m->width); // hsync start
	xout16(out, (uint16_t)m->width); // hsync end
	xout16(out, (uint16_t)m->width); // htotal
	xout16(out, 0); // hskew
	xout16(out, (uint16_t)m->height); // vsync start
	xout16(out, (uint16_t)m->height); // vsync end
	xout16(out, (uint16_t)m->height); // vtotal
	xout16(out, (uint16_t)mode_name(m, name, sizeof(name)));
	xout32(out, 0); // flags
}

static void reply_status(struct xs_client *client, uint32_t timestamp) {
	struct xout out;
	xs_reply_begin(client, &out, 0); // Success
	xout32(&out, timestamp);
	xs_reply_send(client, &out);
}

// ---- requests ----

static void query_version(struct xs_client *client, const struct xin *in) {
	uint32_t major = xin32(in, 4), minor = xin32(in, 8);
	if (major > MAJOR || (major == MAJOR && minor > MINOR)) {
		major = MAJOR;
		minor = MINOR;
	}
	struct xout out;
	xs_reply_begin(client, &out, 0);
	xout32(&out, major);
	xout32(&out, minor);
	xs_reply_send(client, &out);
}

static void select_input(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	uint32_t window = xin32(in, 4);
	uint16_t mask = xin16(in, 8);
	if (!xserver_window(xserver, window)) {
		xs_error(client, XS_ERROR_WINDOW, window, XS_RANDR_OPCODE, 4);
		return;
	}
	struct select *select, *tmp;
	wl_list_for_each_safe(select, tmp, &xserver->randr_selects, link) {
		if (select->client == client && select->window == window) {
			if (mask) {
				select->mask = mask;
			} else {
				wl_list_remove(&select->link);
				free(select);
			}
			return;
		}
	}
	if (!mask) {
		return;
	}
	select = calloc(1, sizeof(*select));
	if (!select) {
		xs_error(client, XS_ERROR_ALLOC, 0, XS_RANDR_OPCODE, 4);
		return;
	}
	select->client = client;
	select->window = window;
	select->mask = mask;
	wl_list_insert(&xserver->randr_selects, &select->link);
}

// RandR 1.0's view: one screen size, the root's, at the fastest rate
static void get_screen_info(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	uint32_t window = xin32(in, 4);
	if (!xserver_window(xserver, window)) {
		xs_error(client, XS_ERROR_WINDOW, window, XS_RANDR_OPCODE, 5);
		return;
	}
	size_t len;
	const struct xs_monitor *all = monitors(xserver, &len);
	int rate = 0;
	for (size_t i = 0; i < len; i++) {
		rate = refresh_of(&all[i]) > rate ? refresh_of(&all[i]) : rate;
	}
	struct xs_window *root = xserver->root;
	struct xout out;
	xs_reply_begin(client, &out, ROTATE_0); // rotations
	xout32(&out, XS_ROOT);
	xout32(&out, xserver->monitors_time);
	xout32(&out, xserver->monitors_time);
	xout16(&out, 1); // sizes
	xout16(&out, 0); // the current one
	xout16(&out, ROTATE_0);
	xout16(&out, (uint16_t)((rate + 500) / 1000));
	xout16(&out, 2); // rate entries: the count, then the rate
	xout16(&out, 0);
	xout16(&out, root->width);
	xout16(&out, root->height);
	xout16(&out, (uint16_t)mm_of(root->width));
	xout16(&out, (uint16_t)mm_of(root->height));
	xout16(&out, 1);
	xout16(&out, (uint16_t)((rate + 500) / 1000));
	xs_reply_send(client, &out);
}

static void set_screen_config(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	struct xout out;
	xs_reply_begin(client, &out, 0); // Success, and nothing changes
	xout32(&out, xserver->monitors_time);
	xout32(&out, xserver->monitors_time);
	xout32(&out, XS_ROOT);
	xout16(&out, 0); // subpixel order unknown
	xs_reply_send(client, &out);
}

static void get_screen_size_range(struct xs_client *client) {
	struct xout out;
	xs_reply_begin(client, &out, 0);
	xout16(&out, 1);
	xout16(&out, 1);
	xout16(&out, 32767);
	xout16(&out, 32767);
	xs_reply_send(client, &out);
}

static void get_screen_resources(struct xs_client *client, const struct xin *in,
		uint8_t minor) {
	struct flame_xserver *xserver = client->xserver;
	uint32_t window = xin32(in, 4);
	if (!xserver_window(xserver, window)) {
		xs_error(client, XS_ERROR_WINDOW, window, XS_RANDR_OPCODE, minor);
		return;
	}
	size_t len;
	const struct xs_monitor *all = monitors(xserver, &len);
	char names[1024];
	size_t names_len = 0;
	for (size_t i = 0; i < len; i++) {
		names_len += mode_name(&all[i], names + names_len, sizeof(names) - names_len);
	}
	struct xout out;
	xs_reply_begin(client, &out, 0);
	xout32(&out, xserver->monitors_time);
	xout32(&out, xserver->monitors_time);
	xout16(&out, (uint16_t)len); // crtcs
	xout16(&out, (uint16_t)len); // outputs
	xout16(&out, (uint16_t)len); // modes
	xout16(&out, (uint16_t)names_len);
	xout_zero(&out, 8);
	for (size_t i = 0; i < len; i++) {
		xout32(&out, all[i].id + 1);
	}
	for (size_t i = 0; i < len; i++) {
		xout32(&out, all[i].id);
	}
	for (size_t i = 0; i < len; i++) {
		put_mode(&out, &all[i]);
	}
	xout_bytes(&out, names, names_len);
	xs_reply_send(client, &out);
}

static void get_output_info(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	uint32_t output = xin32(in, 4);
	const struct xs_monitor *m = find(xserver, output);
	if (!m) {
		xs_error(client, BAD_OUTPUT, output, XS_RANDR_OPCODE, 9);
		return;
	}
	size_t name_len = strlen(m->name);
	struct xout out;
	xs_reply_begin(client, &out, 0); // Success
	xout32(&out, xserver->monitors_time);
	xout32(&out, m->id + 1); // crtc
	xout32(&out, mm_width(m));
	xout32(&out, mm_height(m));
	xout8(&out, 0); // connected
	xout8(&out, 0); // subpixel order unknown
	xout16(&out, 1); // crtcs
	xout16(&out, 1); // modes
	xout16(&out, 1); // preferred: the first
	xout16(&out, 0); // clones
	xout16(&out, (uint16_t)name_len);
	xout32(&out, m->id + 1);
	xout32(&out, m->id + 2);
	xout_bytes(&out, m->name, name_len);
	xs_reply_send(client, &out);
}

// no properties: no EDID, no backlight
static void get_output_property(struct xs_client *client, const struct xin *in) {
	uint32_t output = xin32(in, 4);
	if (!find(client->xserver, output)) {
		xs_error(client, BAD_OUTPUT, output, XS_RANDR_OPCODE, 15);
		return;
	}
	struct xout out;
	xs_reply_begin(client, &out, 0); // format
	xout32(&out, 0); // type None
	xout32(&out, 0); // bytes after
	xout32(&out, 0); // items
	xs_reply_send(client, &out);
}

static void get_crtc_info(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	uint32_t crtc = xin32(in, 4);
	const struct xs_monitor *m = find(xserver, crtc - 1);
	if (!m) {
		xs_error(client, BAD_CRTC, crtc, XS_RANDR_OPCODE, 20);
		return;
	}
	struct xout out;
	xs_reply_begin(client, &out, 0); // Success
	xout32(&out, xserver->monitors_time);
	xout16(&out, (uint16_t)m->x);
	xout16(&out, (uint16_t)m->y);
	xout16(&out, (uint16_t)m->width);
	xout16(&out, (uint16_t)m->height);
	xout32(&out, m->id + 2);
	xout16(&out, ROTATE_0);
	xout16(&out, ROTATE_0);
	xout16(&out, 1); // outputs
	xout16(&out, 1); // possible outputs
	xout32(&out, m->id);
	xout32(&out, m->id);
	xs_reply_send(client, &out);
}

// a straight line, games ask before they fade
static void get_crtc_gamma(struct xs_client *client, const struct xin *in, bool size_only) {
	uint32_t crtc = xin32(in, 4);
	uint8_t minor = size_only ? 22 : 23;
	if (!find(client->xserver, crtc - 1)) {
		xs_error(client, BAD_CRTC, crtc, XS_RANDR_OPCODE, minor);
		return;
	}
	struct xout out;
	xs_reply_begin(client, &out, 0);
	xout16(&out, GAMMA_SIZE);
	xout_zero(&out, 22);
	for (int channel = 0; !size_only && channel < 3; channel++) {
		for (int i = 0; i < GAMMA_SIZE; i++) {
			xout16(&out, (uint16_t)(i * 65535 / (GAMMA_SIZE - 1)));
		}
	}
	xs_reply_send(client, &out);
}

static void put_identity(struct xout *out) {
	for (int i = 0; i < 9; i++) {
		xout32(out, i % 4 == 0 ? 0x10000 : 0);
	}
}

static void get_crtc_transform(struct xs_client *client, const struct xin *in) {
	uint32_t crtc = xin32(in, 4);
	if (!find(client->xserver, crtc - 1)) {
		xs_error(client, BAD_CRTC, crtc, XS_RANDR_OPCODE, 27);
		return;
	}
	struct xout out;
	xs_reply_begin(client, &out, 0);
	put_identity(&out); // pending
	xout8(&out, 1); // has transforms
	xout_zero(&out, 3);
	put_identity(&out); // current
	xout_zero(&out, 4);
	xout_zero(&out, 8); // no filters, pending or current
	xs_reply_send(client, &out);
}

static void get_panning(struct xs_client *client, const struct xin *in) {
	uint32_t crtc = xin32(in, 4);
	if (!find(client->xserver, crtc - 1)) {
		xs_error(client, BAD_CRTC, crtc, XS_RANDR_OPCODE, 28);
		return;
	}
	struct xout out;
	xs_reply_begin(client, &out, 0); // Success
	xout32(&out, client->xserver->monitors_time);
	xout_zero(&out, 24); // panning off
	xs_reply_send(client, &out);
}

static void get_output_primary(struct xs_client *client) {
	size_t len;
	const struct xs_monitor *all = monitors(client->xserver, &len);
	uint32_t primary = 0;
	for (size_t i = 0; i < len; i++) {
		if (all[i].primary) {
			primary = all[i].id;
		}
	}
	struct xout out;
	xs_reply_begin(client, &out, 0);
	xout32(&out, primary);
	xs_reply_send(client, &out);
}

static void get_monitors(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	uint32_t window = xin32(in, 4);
	if (!xserver_window(xserver, window)) {
		xs_error(client, XS_ERROR_WINDOW, window, XS_RANDR_OPCODE, 42);
		return;
	}
	size_t len;
	const struct xs_monitor *all = monitors(xserver, &len);
	struct xout out;
	xs_reply_begin(client, &out, 0);
	xout32(&out, xserver->monitors_time);
	xout32(&out, (uint32_t)len);
	xout32(&out, (uint32_t)len); // outputs, one each
	xout_zero(&out, 12);
	for (size_t i = 0; i < len; i++) {
		const struct xs_monitor *m = &all[i];
		xout32(&out, xs_atom_intern(&xserver->atoms, m->name, strlen(m->name), true));
		xout8(&out, m->primary);
		xout8(&out, 1); // automatic
		xout16(&out, 1);
		xout16(&out, (uint16_t)m->x);
		xout16(&out, (uint16_t)m->y);
		xout16(&out, (uint16_t)m->width);
		xout16(&out, (uint16_t)m->height);
		xout32(&out, mm_width(m));
		xout32(&out, mm_height(m));
		xout32(&out, m->id);
	}
	xs_reply_send(client, &out);
}

void xs_randr_request(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	struct xout out;
	uint8_t minor = xin8(in, 1);
	switch (minor) {
	case 0:
		query_version(client, in);
		break;
	case 2:
		set_screen_config(client, in);
		break;
	case 4:
		select_input(client, in);
		break;
	case 5:
		get_screen_info(client, in);
		break;
	case 6:
		get_screen_size_range(client);
		break;
	case 8: case 25: // GetScreenResources, GetScreenResourcesCurrent
		get_screen_resources(client, in, minor);
		break;
	case 9:
		get_output_info(client, in);
		break;
	case 10: // ListOutputProperties: none
		xs_reply_begin(client, &out, 0);
		xout16(&out, 0);
		xs_reply_send(client, &out);
		break;
	case 11: // QueryOutputProperty: there are none to ask about
		xs_error(client, XS_ERROR_NAME, xin32(in, 8), XS_RANDR_OPCODE, minor);
		break;
	case 15:
		get_output_property(client, in);
		break;
	case 16: // CreateMode: no making up modes
		xs_error(client, XS_ERROR_ACCESS, 0, XS_RANDR_OPCODE, minor);
		break;
	case 20:
		get_crtc_info(client, in);
		break;
	case 21: // SetCrtcConfig: accepted, the outputs stay as flame has them
	case 29: // SetPanning
		reply_status(client, xserver->monitors_time);
		break;
	case 22: case 23:
		get_crtc_gamma(client, in, minor == 22);
		break;
	case 27:
		get_crtc_transform(client, in);
		break;
	case 28:
		get_panning(client, in);
		break;
	case 31:
		get_output_primary(client);
		break;
	case 32: // GetProviders: none
		xs_reply_begin(client, &out, 0);
		xout32(&out, xserver->monitors_time);
		xout16(&out, 0);
		xs_reply_send(client, &out);
		break;
	case 42:
		get_monitors(client, in);
		break;
	// accepted, nothing changes: SetScreenSize, output properties, modes
	// on outputs, gamma, transforms, the primary output, monitors
	case 7: case 12: case 13: case 14: case 17: case 18: case 19: case 24: case 26:
	case 30: case 43: case 44:
		break;
	default:
		xs_error(client, XS_ERROR_REQUEST, 0, XS_RANDR_OPCODE, minor);
		break;
	}
}

// ---- telling listeners ----

static void send_screen_change(struct flame_xserver *xserver, struct select *select) {
	struct xs_window *root = xserver->root;
	struct xs_event event = {{XS_RANDR_FIRST_EVENT, ROTATE_0}};
	xs_event_put32(&event, 4, xserver->monitors_time);
	xs_event_put32(&event, 8, xserver->monitors_time);
	xs_event_put32(&event, 12, XS_ROOT);
	xs_event_put32(&event, 16, select->window);
	xs_event_put16(&event, 20, 0); // size id
	xs_event_put16(&event, 22, 0); // subpixel order
	xs_event_put16(&event, 24, root->width);
	xs_event_put16(&event, 26, root->height);
	xs_event_put16(&event, 28, (uint16_t)mm_of(root->width));
	xs_event_put16(&event, 30, (uint16_t)mm_of(root->height));
	xs_event_send(select->client, &event);
}

static void send_crtc_change(struct flame_xserver *xserver, struct select *select,
		const struct xs_monitor *m) {
	struct xs_event event = {{XS_RANDR_FIRST_EVENT + 1, 0}};
	xs_event_put32(&event, 4, xserver->monitors_time);
	xs_event_put32(&event, 8, select->window);
	xs_event_put32(&event, 12, m->id + 1);
	xs_event_put32(&event, 16, m->id + 2);
	xs_event_put16(&event, 20, ROTATE_0);
	xs_event_put16(&event, 24, (uint16_t)m->x);
	xs_event_put16(&event, 26, (uint16_t)m->y);
	xs_event_put16(&event, 28, (uint16_t)m->width);
	xs_event_put16(&event, 30, (uint16_t)m->height);
	xs_event_send(select->client, &event);
}

static void send_output_change(struct flame_xserver *xserver, struct select *select,
		const struct xs_monitor *m) {
	struct xs_event event = {{XS_RANDR_FIRST_EVENT + 1, 1}};
	xs_event_put32(&event, 4, xserver->monitors_time);
	xs_event_put32(&event, 8, xserver->monitors_time);
	xs_event_put32(&event, 12, select->window);
	xs_event_put32(&event, 16, m->id);
	xs_event_put32(&event, 20, m->id + 1);
	xs_event_put32(&event, 24, m->id + 2);
	xs_event_put16(&event, 28, ROTATE_0);
	xs_event_put8(&event, 30, 0); // connected
	xs_event_put8(&event, 31, 0);
	xs_event_send(select->client, &event);
}

static void send_resource_change(struct flame_xserver *xserver, struct select *select) {
	struct xs_event event = {{XS_RANDR_FIRST_EVENT + 1, 5}};
	xs_event_put32(&event, 4, xserver->monitors_time);
	xs_event_put32(&event, 8, select->window);
	xs_event_send(select->client, &event);
}

void xs_randr_changed(struct flame_xserver *xserver) {
	size_t len;
	const struct xs_monitor *all = monitors(xserver, &len);
	struct select *select;
	wl_list_for_each(select, &xserver->randr_selects, link) {
		for (size_t i = 0; i < len; i++) {
			if (select->mask & CRTC_CHANGE_MASK) {
				send_crtc_change(xserver, select, &all[i]);
			}
			if (select->mask & OUTPUT_CHANGE_MASK) {
				send_output_change(xserver, select, &all[i]);
			}
		}
		if (select->mask & RESOURCE_CHANGE_MASK) {
			send_resource_change(xserver, select);
		}
		// last, toolkits re-read everything when it comes
		if (select->mask & SCREEN_CHANGE_MASK) {
			send_screen_change(xserver, select);
		}
	}
}

void xs_randr_client_gone(struct flame_xserver *xserver, struct xs_client *client) {
	struct select *select, *tmp;
	wl_list_for_each_safe(select, tmp, &xserver->randr_selects, link) {
		if (!client || select->client == client) {
			wl_list_remove(&select->link);
			free(select);
		}
	}
}

void xs_randr_window_gone(struct flame_xserver *xserver, uint32_t window) {
	struct select *select, *tmp;
	wl_list_for_each_safe(select, tmp, &xserver->randr_selects, link) {
		if (select->window == window) {
			wl_list_remove(&select->link);
			free(select);
		}
	}
}
