#ifndef FLAME_WALLPAPER_WALLPAPER_H
#define FLAME_WALLPAPER_WALLPAPER_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

#include "config/config.h"

struct flame_server;
struct flame_output;

// the config's wallpaper on every screen, over the background color and
// under everything else, layer-shell wallpapers (swaybg, awww) included.
// a worker thread reads the picture and scales it to each screen's pixels
// (a 6k photo never holds up a frame), the screen gets it as one texture
// and fades it in over the one before

struct flame_wallpaper {
	struct flame_server *server;
	struct wl_list screens; // wallpaper_screen.link
	uint64_t next_id;
	// what the screens show now: a change of any of these makes them again
	char *path; // NULL for none
	enum flame_wallpaper_mode mode;
	float color[4];

	// the worker. jobs and done under lock, it says it finished one down notify
	pthread_t thread;
	bool thread_up, stop;
	pthread_mutex_t lock;
	pthread_cond_t wake;
	struct wl_list jobs, done; // wallpaper_job.link
	int notify[2];
	struct wl_event_source *notify_source;
};

bool wallpaper_init(struct flame_wallpaper *wallpaper, struct flame_server *server);
void wallpaper_finish(struct flame_wallpaper *wallpaper);

// output came, moved, or changed its size, scale or turn: its picture
// follows, made again only when its pixels changed
void wallpaper_output_changed(struct flame_wallpaper *wallpaper, struct flame_output *output);
void wallpaper_output_gone(struct flame_wallpaper *wallpaper, struct flame_output *output);
// the config was loaded again: a new picture, mode or background color
void wallpaper_config_changed(struct flame_wallpaper *wallpaper);

#endif
