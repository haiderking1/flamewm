#ifndef FLAME_INPUT_MEDIA_H
#define FLAME_INPUT_MEDIA_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>

// the media keys (volume, brightness, mute, play) go to whoever asked for
// them over ipc (the bar: it changes them and shows it), volume and
// brightness again and again while they're held, like typing does. nobody
// asked: they go to the window like any key

struct flame_server;

struct flame_media_keys {
	struct flame_server *server;
	struct wl_event_source *repeat;
	xkb_keycode_t held; // repeating, 0 for none
	const char *held_name;
};

bool media_keys_init(struct flame_media_keys *keys, struct flame_server *server);
void media_keys_finish(struct flame_media_keys *keys);
// a key went down or up: true when it was a media key someone took
bool media_key(struct flame_media_keys *keys, xkb_keycode_t keycode, xkb_keysym_t sym,
	bool pressed);

#endif
