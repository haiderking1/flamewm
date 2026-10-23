#ifndef FLAMEBAR_CONTROL_H
#define FLAMEBAR_CONTROL_H

#include "../flameui/flameui.h"
#include "bluetooth.h"
#include "look.h"
#include "media.h"
#include "network.h"
#include "notify.h"
#include "sound.h"

// the control center: tiles of glass under the bar's button, what each
// shows from its source, a click there changes it

struct control {
	struct sound *sound;
	struct media *media;
	struct network *network;
	struct bluetooth *bluetooth;
	struct notify *notify; // do not disturb's
	int dragging; // the slider held: an enum sound_side, -1 for none
	int picking; // whose devices are listed: an enum sound_side, -1 for none
	bool media_open; // now playing big
	double room; // how tall it can be on the surface now: what grows waits for it
	bool scrubbing; // its place in what plays held
	unsigned ticks;
};

#define CONTROL_W 340

// a crescent at cx, cy in a s box: do not disturb
void icon_moon(cairo_t *cr, double cx, double cy, double s);

// how tall it is now (tiles come and go with what there is)
double control_height(struct control *control);
// drawn with its top right at right, top: its glass into panes (as many as
// fit in max), how many
int control_frame(struct fui *ui, struct control *control, double right, double top,
	struct fui_pane *panes, int max);

#endif
