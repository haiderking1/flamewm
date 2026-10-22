#ifndef FLAMEFILES_ICONS_H
#define FLAMEFILES_ICONS_H

#include "../flameui/flameui.h"
#include "folder.h"

// flamefiles draws its own icons: blue folders, white pages with their
// kind's color and extension on them, small line glyphs for the sidebar

enum place {
	PLACE_NONE = -1,
	PLACE_HOME,
	PLACE_DESKTOP,
	PLACE_DOCUMENTS,
	PLACE_DOWNLOADS,
	PLACE_PICTURES,
	PLACE_MUSIC,
	PLACE_VIDEOS,
	PLACE_FOLDER,
	PLACE_COMPUTER,
	PLACE_TRASH,
};

// a folder s big (a square box at x, y), a place's glyph on its front
void icon_folder(struct fui *ui, double x, double y, double s, enum place place);
// a file of kind called name s big: a page, its kind's band and extension
void icon_file(struct fui *ui, double x, double y, double s, enum file_kind kind,
	const char *name);
// a picture of the file (a thumbnail) fit in the box s big, framed
void icon_picture(struct fui *ui, double x, double y, double s, cairo_surface_t *thumb);
// a sidebar glyph, s big, in color
void icon_place(struct fui *ui, enum place place, double x, double y, double s, uint32_t color);

#endif
