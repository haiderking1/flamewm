#include "xshell/xsettings.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xserver/xserver.h"
#include "xshell/xshell.h"

// an XSETTINGS integer, the property's way: type 0, name padded to 4, the
// serial it last changed at, the value
static size_t put_int(uint8_t *out, const char *name, uint32_t serial, int32_t value) {
	size_t len = strlen(name);
	size_t padded = (len + 3) & ~(size_t)3;
	uint16_t len16 = (uint16_t)len;
	size_t at = 0;
	out[at++] = 0; // integer
	out[at++] = 0;
	memcpy(out + at, &len16, 2);
	at += 2;
	memset(out + at, 0, padded);
	memcpy(out + at, name, len);
	at += padded;
	memcpy(out + at, &serial, 4);
	at += 4;
	memcpy(out + at, &value, 4);
	return at + 4;
}

// RESOURCE_MANAGER with Xft.dpi as dpi (0: without it). what else is
// there stays, line by line
static void set_xft_dpi(struct flame_xshell *xshell, int dpi) {
	struct flame_xserver *xserver = xshell->xserver;
	struct xs_property *p = xserver_property(xserver, xserver->root, "RESOURCE_MANAGER");
	size_t old_len = p && p->format == 8 ? p->units : 0;
	char *out = malloc(old_len + 64);
	if (!out) {
		return;
	}
	size_t len = 0;
	const char *data = old_len ? (const char *)p->data : "";
	for (size_t at = 0; at < old_len;) {
		const char *nl = memchr(data + at, '\n', old_len - at);
		size_t line = nl ? (size_t)(nl - (data + at)) + 1 : old_len - at;
		if (strncmp(data + at, "Xft.dpi:", 8)) {
			memcpy(out + len, data + at, line);
			len += line;
			if (!nl) {
				out[len++] = '\n';
			}
		}
		at += line;
	}
	if (dpi > 0) {
		len += (size_t)snprintf(out + len, 64, "Xft.dpi:\t%d\n", dpi);
	}
	uint32_t name = xserver_atom(xserver, "RESOURCE_MANAGER");
	if (len) {
		xserver_set_property(xserver, xserver->root, name, xserver_atom(xserver, "STRING"), 8,
			out, (uint32_t)len);
	} else {
		xserver_delete_property(xserver, xserver->root, name);
	}
	free(out);
}

void xsettings_update(struct flame_xshell *xshell, bool on, double scale) {
	struct flame_xserver *xserver = xshell->xserver;
	struct xs_window *owner = xserver_window(xserver, XS_FLAME_WINDOW);
	uint32_t selection = xserver_atom(xserver, "_XSETTINGS_S0");
	uint32_t settings = xserver_atom(xserver, "_XSETTINGS_SETTINGS");
	if (!owner) {
		return;
	}
	if (!on) {
		if (xshell->xsettings_on) {
			xserver_selection_own(xserver, selection, false);
			xserver_delete_property(xserver, owner, settings);
			set_xft_dpi(xshell, 0);
			xshell->xsettings_on = false;
		}
		return;
	}
	// GTK scales by whole numbers, the fraction left over goes in the dpi
	int factor = scale >= 1 ? (int)floor(scale) : 1;
	int dpi = (int)lround(96.0 * scale);
	uint32_t serial = ++xshell->xsettings_serial;
	uint8_t data[256];
	size_t len = 12;
	data[0] = 0; // little endian, as flame's X is
	memset(data + 1, 0, 3);
	memcpy(data + 4, &serial, 4);
	uint32_t count = 3;
	memcpy(data + 8, &count, 4);
	len += put_int(data + len, "Xft/DPI", serial, dpi * 1024);
	len += put_int(data + len, "Gdk/WindowScalingFactor", serial, factor);
	len += put_int(data + len, "Gdk/UnscaledDPI", serial,
		(int32_t)lround(96.0 * 1024 * scale / factor));
	xserver_set_property(xserver, owner, settings, settings, 8, data, (uint32_t)len);
	if (!xshell->xsettings_on) {
		xserver_selection_own(xserver, selection, true);
		xshell->xsettings_on = true;
	}
	set_xft_dpi(xshell, dpi);
}
