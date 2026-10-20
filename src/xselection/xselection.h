#ifndef FLAME_XSELECTION_XSELECTION_H
#define FLAME_XSELECTION_XSELECTION_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct flame_server;
struct flame_xserver;
struct xs_selection_request;
struct xs_transfer;
struct xsel_source;
struct xsel_dnd;
struct wlr_drag;

// the clipboard and the primary selection between X apps and wayland apps.
// an X app copying: flame asks it what it has (TARGETS), offers that to
// wayland as a data source, and fetches the real thing when a wayland app
// pastes. a wayland app copying: flame owns the selection on the X side and
// answers X apps from the wayland source. text, images, files: anything
// with a mime type, under the atom of the same name (utf-8 text is
// UTF8_STRING, the way everyone has it)

// one selection, the clipboard or the primary one
struct xsel {
	struct flame_xselection *owner;
	uint32_t atom;
	bool primary;
	struct xs_transfer *targets; // asking the X owner what it has
	struct xsel_source *source; // what the X owner has, offered to wayland
};

struct flame_xselection {
	struct flame_server *server;
	struct flame_xserver *xserver;
	struct xsel clipboard, primary;
	struct wl_list moves; // xsel_move.link, data on its way somewhere
	struct xsel_dnd *dnd; // drag and drop, see xdnd.c
	struct wl_listener set_selection, set_primary_selection;
	uint32_t targets, timestamp, utf8_string, text, string, atom, integer;
};

bool xselection_init(struct flame_xselection *xselection, struct flame_server *server,
	struct flame_xserver *xserver);
void xselection_finish(struct flame_xselection *xselection);

// from the X server's hooks
void xselection_owner_changed(struct flame_xselection *xselection, uint32_t selection,
	bool x_owned);
void xselection_request(struct flame_xselection *xselection,
	struct xs_selection_request *request);
void xselection_client_message(struct flame_xselection *xselection, uint32_t window,
	uint32_t type, const uint32_t data[5]);

// drag and drop (xdnd.c). the pointer moved: an X app's drag over wayland
// gets the proxy that hears it
void xselection_pointer_moved(struct flame_xselection *xselection);
// a wayland drag with no wayland surface under the pointer: true when an X
// window there takes it (XDND)
bool xselection_dnd_over_x(struct flame_xselection *xselection, double lx, double ly,
	uint32_t time);
// a wayland drag is over a wayland surface again
void xselection_dnd_left_x(struct flame_xselection *xselection);
// the wayland drag ended without a drop on X (cancelled, its client gone)
void xselection_dnd_drag_gone(struct flame_xselection *xselection);
// the button let go during a wayland drag: true when it dropped on an X
// window, the seat heard the release already
bool xselection_dnd_drop_x(struct flame_xselection *xselection, uint32_t button,
	uint32_t time);
// a drag flame steers for an X app, not the pointer's
bool xselection_drag_is_x(struct wlr_drag *drag);

#endif
