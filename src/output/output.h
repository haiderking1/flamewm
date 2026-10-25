#ifndef FLAME_OUTPUT_OUTPUT_H
#define FLAME_OUTPUT_OUTPUT_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/util/box.h>

#include "render/frame.h"

struct flame_server;
struct flame_rect;

struct flame_output {
	struct wl_list link; // flame_outputs.list
	struct flame_server *server;
	struct wlr_output *wlr;
	struct flame_frame frames; // what it showed last and what changed since
	struct flame_rect *background;
	struct wlr_box box; // layout coords
	struct wlr_box usable; // box minus what bars reserve, tiles go here
	// the mode on it is the one flame.lua asked for: without the ask any more
	// it goes back to the fastest
	bool mode_asked;

	struct wl_listener request_state;
	struct wl_listener destroy;
};

struct flame_outputs {
	struct flame_server *server;
	struct wlr_output_layout *layout;
	struct wl_list list; // flame_output.link, in plug order

	struct wl_listener new_output;
	struct wl_listener layout_change;
	struct wl_listener hotplug;

	struct {
		// an output's frames are set up, struct flame_frame *
		struct wl_signal new_frame;
	} events;
};

bool outputs_init(struct flame_outputs *outputs, struct flame_server *server);
void outputs_finish(struct flame_outputs *outputs);

// ask every output for a frame, the schedule decides when
void outputs_schedule_frame(struct flame_outputs *outputs);

// single output for now, the first one plugged in owns the desktop
struct flame_output *outputs_primary(struct flame_outputs *outputs);

// flame.lua changed: scales follow it
void outputs_apply_config(struct flame_outputs *outputs);

// tests (FLAME_TEST_INPUT=1, headless): every screen goes, the way some
// drivers drop them on a vt switch, and one comes back
void outputs_test_unplug(struct flame_outputs *outputs);
bool outputs_test_plug(struct flame_outputs *outputs);

#endif
