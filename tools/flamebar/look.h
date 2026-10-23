#ifndef FLAMEBAR_LOOK_H
#define FLAMEBAR_LOOK_H

#include "../flameui/flameui.h"

// how the bar's things look, the same everywhere: white text with a soft
// dark edge under it, tiles and cards of clear glass

#define TEXT 0xffffffff
#define TEXT_DIM 0xffffffc7
#define ON 0x0a84ffff // a switch that's on
#define F_TITLE "Inter Semi-Bold 10.5"
#define F_BIG_TITLE "Inter Semi-Bold 12"
#define F_SUB "Inter Medium 9.5"
#define F_BODY "Inter 10"
#define F_LIST_TITLE "Inter Semi-Bold 8.5"

// glass like real glass: clear (what's behind it softened, not fogged, a
// touch dimmed so white reads on a bright wallpaper), its rim a lens
// bending what's under it, barely lit, one piece
struct fui_pane glass(double x, double y, double w, double h, double r);

// text that reads over glass or the wallpaper: a soft dark edge under it.
// how wide it is
double shadowed_text(struct fui *ui, double x, double y, const char *text, const char *font,
	uint32_t color, int align, double max_width);

#endif
