#ifndef FLAME_DESKTOP_DESKTOP_H
#define FLAME_DESKTOP_DESKTOP_H

#include <stdbool.h>
#include <wlr/util/box.h>

#include "desktop/focus.h"
#include "desktop/fullscreen.h"
#include "desktop/view.h"
#include "desktop/workspace.h"
#include "transaction/transaction.h"

struct flame_server;
struct wlr_surface;

// owns every window and workspace and decides where things go
struct flame_desktop {
	struct flame_server *server;
	struct flame_tree *staging; // disabled, unmapped views wait here
	struct flame_workspace workspaces[FLAME_WORKSPACE_COUNT];
	struct flame_workspace *active;
	struct flame_view *focused;
	struct flame_transaction transaction;

	// something a bar would show changed: workspaces, windows, focus, titles.
	// fires at most once per loop iteration, after the dust settles
	struct wl_event_source *changed_idle;
	struct {
		struct wl_signal changed;
	} events;
};

bool desktop_init(struct flame_desktop *desktop, struct flame_server *server);
void desktop_finish(struct flame_desktop *desktop);

// let whoever listens on events.changed know, soon
void desktop_notify(struct flame_desktop *desktop);

// lay out the visible workspace again, call after anything moves
void desktop_arrange(struct flame_desktop *desktop);
// same, but everything lands right now and nobody waits on clients. for a
// mouse resize: the edges follow the cursor and clients catch up when they can
void desktop_arrange_live(struct flame_desktop *desktop);
// the live arranging is over, windows it resized get their final size for real
void desktop_end_live(struct flame_desktop *desktop);
// a transaction just put things in place
void desktop_layout_applied(struct flame_desktop *desktop);

// where a window opening right now would land, so its very first configure
// already has the final size. false when there's no output to put it on
bool desktop_prospective_geometry(struct flame_desktop *desktop, struct wlr_box *box);

void desktop_map_view(struct flame_desktop *desktop, struct flame_view *view);
void desktop_unmap_view(struct flame_desktop *desktop, struct flame_view *view);

// swap two windows on the visible workspace, lands as one transaction
void desktop_swap_views(struct flame_desktop *desktop, struct flame_view *a,
	struct flame_view *b);

void desktop_switch_workspace(struct flame_desktop *desktop, int index);
void desktop_move_view_to_workspace(struct flame_desktop *desktop,
	struct flame_view *view, int index);

// view under a layout point. surface/sx/sy are only set over client content,
// a border hit gives you the view with a NULL surface
struct flame_view *desktop_view_at(struct flame_desktop *desktop, double lx, double ly,
	struct wlr_surface **surface, double *sx, double *sy);

#endif
