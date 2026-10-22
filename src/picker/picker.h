#ifndef FLAME_PICKER_PICKER_H
#define FLAME_PICKER_PICKER_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wlr/util/box.h>
#include <xkbcommon/xkbcommon.h>

#include "anim/anim.h"

struct flame_server;
struct flame_ipc_client;
struct flame_view;
struct flame_tree;
struct flame_rect;
struct flame_glass;
struct flame_buffer_node;
struct wlr_texture;

// what to share, picked on the windows themselves.
// the screen dims but for the window under the pointer, and a glass panel on
// it says what a click does. a glass bar on top switches between a window
// and the whole screen, or cancels. super and the workspace keys still work:
// a window anywhere can be picked. the screen sharing portal runs `flamectl
// share-pick` to ask, the answer goes back on that line: "Monitor: NAME" or
// "Window: ID" (ext-foreign-toplevel's identifier), an error for cancelled

enum picker_mode {
	PICKER_WINDOW,
	PICKER_SCREEN,
};

// a line of text in the scene
struct picker_label {
	struct flame_buffer_node *node;
	struct wlr_texture *texture;
	int width, height;
};

#define PICKER_CHOICES 3 // the bar: a window, the screen, cancel

struct flame_picker {
	struct flame_server *server;
	struct flame_tree *tree; // above everything, off while closed
	struct flame_rect *dim[4]; // around what's lit
	struct flame_rect *ring[4]; // a thin light line around it

	// the bar: glass, a brighter knob behind the mode it's in, the labels
	struct flame_tree *bar;
	struct flame_glass *bar_glass, *knob;
	struct picker_label choices[PICKER_CHOICES];
	struct wlr_box choice_boxes[PICKER_CHOICES]; // layout coords
	struct wlr_box bar_box;
	// the panel on what's lit: what it is, what a click does
	struct flame_tree *panel;
	struct flame_glass *panel_glass;
	struct picker_label title, action;

	enum picker_mode mode;
	struct flame_view *view; // the window lit, NULL for none
	bool open; // taking input, waiting for a pick
	struct wlr_box area; // the output it's on, layout coords
	float scale;

	// what's lit (layout, empty for nothing) and the knob, gliding to where
	// they go next
	struct wlr_box lit, lit_from, lit_to;
	struct wlr_box knob_box, knob_from, knob_to;
	struct flame_anim glide;
	struct flame_anim fade; // opening and closing
	double shown, shown_from, shown_to; // 0 closed .. 1 open

	struct flame_ipc_client *client;
	struct wl_listener client_destroy;
	// windows move, close, the user switches workspaces: what's lit follows
	struct wl_event_source *refresh;
	uint8_t eaten[32]; // keys it took the press of, a bit a keycode: their release too
};

bool picker_init(struct flame_picker *picker, struct flame_server *server);
void picker_finish(struct flame_picker *picker);

// shows it for client, whose answer comes when the user picks or cancels.
// one open already: that one's cancelled
void picker_open(struct flame_picker *picker, struct flame_ipc_client *client);
// open and taking the pointer and the keyboard
bool picker_active(const struct flame_picker *picker);

void picker_pointer_motion(struct flame_picker *picker, double lx, double ly);
void picker_pointer_button(struct flame_picker *picker, double lx, double ly, bool pressed);
// a key, xkb keycode, with the modifiers held (wlr_keyboard_modifier). true:
// the picker took it. a press while it's open is its, and that key's release
// after, even once it's closed. with ctrl, alt or super held it goes on to
// the binds: switching workspaces works while it's open
bool picker_key(struct flame_picker *picker, uint32_t keycode, xkb_keysym_t sym,
	bool pressed, uint32_t modifiers);

// a window it shows is going away
void picker_view_gone(struct flame_picker *picker, struct flame_view *view);

#endif
