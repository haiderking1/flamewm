#ifndef FLAMEPLAY_PLAYER_H
#define FLAMEPLAY_PLAYER_H

#include <stdbool.h>
#include <stddef.h>

// flameplay's engine: a file read and decoded on threads of its own (ffmpeg),
// its sound played through pipewire, its frames turned into pictures the
// window shows when they're due. the sound is the clock the picture
// follows (a file without sound: the wall clock)

// a picture a frame goes into: the window's (a fui_picture), handed to the
// engine to fill
struct player_picture {
	void *pixels;
	int stride;
	void *data; // the window's own
	// the engine's
	double pts; // seconds from the start
	int serial; // which seek it's from
	int state;
};

struct player;

// path opened, wake (from any thread) when something's ready: a frame, the
// end. NULL with error when it can't be played
struct player *player_open(const char *path, void (*wake)(void *data), void *data, char *error,
	size_t error_size);
void player_close(struct player *player);

// what it is: its frames' pixels and how wide they show (wide pixels made
// square), how long, a title (its tags', else NULL)
void player_size(const struct player *player, int *width, int *height);
double player_aspect(const struct player *player);
double player_duration(const struct player *player);
bool player_has_video(const struct player *player);
bool player_has_audio(const struct player *player);
const char *player_title(const struct player *player);

// the pictures frames go into, width x height each (player_size), before
// it plays
void player_set_pictures(struct player *player, struct player_picture *pictures, int count);

void player_play(struct player *player);
void player_pause(struct player *player);
bool player_playing(const struct player *player);
// played to the end (it pauses there)
bool player_ended(const struct player *player);
// to t seconds, exactly that frame
void player_seek(struct player *player, double t);
// where it is now, seconds
double player_time(const struct player *player);
void player_set_volume(struct player *player, double volume);

// the picture to show now: the newest one due (older ones it skips), NULL
// for nothing new. it's the window's till player_picture_done
struct player_picture *player_due(struct player *player);
void player_picture_done(struct player *player, struct player_picture *picture);

#endif
