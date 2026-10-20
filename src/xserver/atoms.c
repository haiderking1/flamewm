#include "xserver/atoms.h"

#include <stdlib.h>
#include <string.h>

// the core protocol's, numbered 1 to 68 in this order
static const char *const predefined[] = {
	"PRIMARY", "SECONDARY", "ARC", "ATOM", "BITMAP", "CARDINAL", "COLORMAP", "CURSOR",
	"CUT_BUFFER0", "CUT_BUFFER1", "CUT_BUFFER2", "CUT_BUFFER3", "CUT_BUFFER4",
	"CUT_BUFFER5", "CUT_BUFFER6", "CUT_BUFFER7", "DRAWABLE", "FONT", "INTEGER", "PIXMAP",
	"POINT", "RECTANGLE", "RESOURCE_MANAGER", "RGB_COLOR_MAP", "RGB_BEST_MAP",
	"RGB_BLUE_MAP", "RGB_DEFAULT_MAP", "RGB_GRAY_MAP", "RGB_GREEN_MAP", "RGB_RED_MAP",
	"STRING", "VISUALID", "WINDOW", "WM_COMMAND", "WM_HINTS", "WM_CLIENT_MACHINE",
	"WM_ICON_NAME", "WM_ICON_SIZE", "WM_NAME", "WM_NORMAL_HINTS", "WM_SIZE_HINTS",
	"WM_ZOOM_HINTS", "MIN_SPACE", "NORM_SPACE", "MAX_SPACE", "END_SPACE", "SUPERSCRIPT_X",
	"SUPERSCRIPT_Y", "SUBSCRIPT_X", "SUBSCRIPT_Y", "UNDERLINE_POSITION",
	"UNDERLINE_THICKNESS", "STRIKEOUT_ASCENT", "STRIKEOUT_DESCENT", "ITALIC_ANGLE",
	"X_HEIGHT", "QUAD_WIDTH", "WEIGHT", "POINT_SIZE", "RESOLUTION", "COPYRIGHT", "NOTICE",
	"FONT_NAME", "FAMILY_NAME", "FULL_NAME", "CAP_HEIGHT", "WM_CLASS", "WM_TRANSIENT_FOR",
};

// not predefined by the protocol, but every X server has them before the
// first client shows up (a window manager interns them). apps ask for some
// with only-if-exists and give up on close buttons and such without them
static const char *const common[] = {
	"WM_PROTOCOLS", "WM_DELETE_WINDOW", "WM_TAKE_FOCUS", "WM_STATE", "WM_CHANGE_STATE",
	"WM_CLIENT_LEADER", "WM_WINDOW_ROLE", "UTF8_STRING", "CLIPBOARD", "TARGETS",
	"_NET_SUPPORTED", "_NET_ACTIVE_WINDOW", "_NET_WM_NAME", "_NET_WM_ICON_NAME",
	"_NET_WM_PID", "_NET_WM_PING", "_NET_WM_STATE", "_NET_WM_STATE_FULLSCREEN",
	"_NET_WM_STATE_HIDDEN", "_NET_WM_STATE_FOCUSED", "_NET_WM_STATE_MAXIMIZED_VERT",
	"_NET_WM_STATE_MAXIMIZED_HORZ", "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_NORMAL",
	"_NET_WM_WINDOW_TYPE_DIALOG", "_NET_WM_BYPASS_COMPOSITOR", "_MOTIF_WM_HINTS",
};

// a client can intern as much as it likes, not without end
#define MAX_ATOMS 65536

static bool push(struct xs_atoms *atoms, const char *name, size_t len) {
	if (atoms->len == atoms->cap) {
		size_t cap = atoms->cap ? atoms->cap * 2 : 256;
		char **names = realloc(atoms->names, cap * sizeof(*names));
		if (!names) {
			return false;
		}
		atoms->names = names;
		atoms->cap = cap;
	}
	char *copy = NULL;
	if (name) {
		copy = strndup(name, len);
		if (!copy) {
			return false;
		}
	}
	atoms->names[atoms->len++] = copy;
	return true;
}

bool xs_atoms_init(struct xs_atoms *atoms) {
	*atoms = (struct xs_atoms){0};
	if (!push(atoms, NULL, 0)) { // None
		return false;
	}
	for (size_t i = 0; i < sizeof(predefined) / sizeof(predefined[0]); i++) {
		if (!push(atoms, predefined[i], strlen(predefined[i]))) {
			xs_atoms_finish(atoms);
			return false;
		}
	}
	for (size_t i = 0; i < sizeof(common) / sizeof(common[0]); i++) {
		if (!push(atoms, common[i], strlen(common[i]))) {
			xs_atoms_finish(atoms);
			return false;
		}
	}
	return true;
}

void xs_atoms_finish(struct xs_atoms *atoms) {
	for (size_t i = 0; i < atoms->len; i++) {
		free(atoms->names[i]);
	}
	free(atoms->names);
	*atoms = (struct xs_atoms){0};
}

uint32_t xs_atom_intern(struct xs_atoms *atoms, const char *name, size_t len, bool create) {
	for (size_t i = 1; i < atoms->len; i++) {
		if (strlen(atoms->names[i]) == len && !memcmp(atoms->names[i], name, len)) {
			return (uint32_t)i;
		}
	}
	if (!create || len == 0 || atoms->len >= MAX_ATOMS || !push(atoms, name, len)) {
		return 0;
	}
	return (uint32_t)(atoms->len - 1);
}

const char *xs_atom_name(const struct xs_atoms *atoms, uint32_t atom) {
	return atom > 0 && atom < atoms->len ? atoms->names[atom] : NULL;
}
