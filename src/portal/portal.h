#ifndef FLAME_PORTAL_PORTAL_H
#define FLAME_PORTAL_PORTAL_H

#include <stdbool.h>
#include <wayland-server-core.h>

struct flame_server;
struct flame_view;

// flame's xdg-desktop-portal backend (org.freedesktop.impl.portal.desktop
// .flamewm): screen sharing and screenshots. xdg-desktop-portal asks it on
// the apps' behalf, flame's own picker asks the user, screencast/ streams
// or shoots what was picked. a share comes back with restore data: the same
// app asking again while it runs (a browser asking twice for one share)
// gets the same thing, no second pick. it's on the session bus when flame
// is the session (a real seat), or with FLAME_PORTAL=1. FLAME_PORTAL=0
// keeps it off
struct flame_portal {
	struct flame_server *server;
	struct sd_bus *bus;
	struct sd_bus_slot *slot, *shot_slot;
	struct wl_event_source *source, *timer;
	struct wl_list sessions; // portal_session.link
};

bool portal_init(struct flame_portal *portal, struct flame_server *server);
void portal_finish(struct flame_portal *portal);

#endif
