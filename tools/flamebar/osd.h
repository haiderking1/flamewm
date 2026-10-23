#ifndef FLAMEBAR_OSD_H
#define FLAMEBAR_OSD_H

#include "../flameui/flameui.h"
#include "control.h"

// what a media key just changed, a moment under the bar: the volume, the
// mic or the screen's brightness as a level on glass, dragged to change it
// more. only for the keys: the control center's sliders don't show it

enum osd_kind { OSD_VOLUME, OSD_MIC, OSD_BRIGHTNESS };

struct osd {
	enum osd_kind kind;
	double level; // 0 to 1
	bool muted;
	double until; // up till then (fui's time)
	bool dragging;
};

#define OSD_W 300
#define OSD_H 50

void osd_show(struct osd *osd, enum osd_kind kind, double level, bool muted, double now);
// its right edge at right, from top (hidden: it goes): its glass into
// panes and where it takes clicks into rects, as in notes.h. how much
// room it takes under top now (what's under it moves down by that), 0 for
// none
double osd_frame(struct fui *ui, struct osd *osd, struct control *control, double right,
	double top, bool hidden, struct fui_pane *panes, int *n_panes, int max_panes,
	struct fui_rect *rects, int *n_rects, int max_rects);

#endif
