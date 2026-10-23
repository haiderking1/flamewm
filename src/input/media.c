#include "input/media.h"

#include <stdio.h>
#include <string.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "config/config.h"
#include "core/server.h"
#include "util/macros.h"

static const struct {
	xkb_keysym_t sym;
	const char *name;
	bool repeats;
} keys[] = {
	{XKB_KEY_XF86AudioRaiseVolume, "volume-up", true},
	{XKB_KEY_XF86AudioLowerVolume, "volume-down", true},
	{XKB_KEY_XF86AudioMute, "mute", false},
	{XKB_KEY_XF86AudioMicMute, "mic-mute", false},
	{XKB_KEY_XF86MonBrightnessUp, "brightness-up", true},
	{XKB_KEY_XF86MonBrightnessDown, "brightness-down", true},
	{XKB_KEY_XF86AudioPlay, "play-pause", false},
	{XKB_KEY_XF86AudioPause, "play-pause", false},
	{XKB_KEY_XF86AudioNext, "next", false},
	{XKB_KEY_XF86AudioPrev, "previous", false},
};

static void stop(struct flame_media_keys *media) {
	media->held = 0;
	wl_event_source_timer_update(media->repeat, 0);
}

static int handle_repeat(void *data) {
	struct flame_media_keys *media = data;
	const struct flame_config *config = media->server->config;
	if (!media->held || !ipc_media_key(&media->server->ipc, media->held_name)) {
		stop(media);
		return 0;
	}
	wl_event_source_timer_update(media->repeat,
		config->repeat_rate > 0 ? 1000 / config->repeat_rate : 40);
	return 0;
}

bool media_key(struct flame_media_keys *media, xkb_keycode_t keycode, xkb_keysym_t sym,
		bool pressed) {
	if (!pressed) {
		if (keycode == media->held) {
			stop(media);
		}
		return false; // the release goes on: whoever got the press is told it's up
	}
	for (size_t i = 0; i < LENGTH(keys); i++) {
		if (keys[i].sym != sym) {
			continue;
		}
		if (!ipc_media_key(&media->server->ipc, keys[i].name)) {
			return false;
		}
		if (keys[i].repeats) {
			media->held = keycode;
			media->held_name = keys[i].name;
			wl_event_source_timer_update(media->repeat, media->server->config->repeat_delay);
		}
		return true;
	}
	return false;
}

bool media_keys_init(struct flame_media_keys *media, struct flame_server *server) {
	media->server = server;
	media->repeat = wl_event_loop_add_timer(server->loop, handle_repeat, media);
	return media->repeat != NULL;
}

void media_keys_finish(struct flame_media_keys *media) {
	if (media->repeat) {
		wl_event_source_remove(media->repeat);
		media->repeat = NULL;
	}
}
