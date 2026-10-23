#ifndef FLAMEPLAY_PLAYER_H
#define FLAMEPLAY_PLAYER_H

#include <stdbool.h>
#include <stddef.h>

#include "gpu.h"

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
// its picture's a song's cover, not a video
bool player_has_cover(const struct player *player);
const char *player_title(const struct player *player);

// decoded on the gpu: its pictures are images there, made by
// player_gpu_images (dmabufs the window hands flame, their pixels not the
// cpu's). flame can't take them: player_use_software, it decodes on the cpu
bool player_gpu(const struct player *player);
bool player_gpu_images(struct player *player, int count, struct gpu_image *out);
void player_use_software(struct player *player);

// its sounds (languages, commentary) and subtitles to pick from: their
// names, which is on. subtitles from the file and from files beside it
// (movie.srt, movie.en.srt), the ones on when it opens: a file beside it,
// else what the file says must be shown
enum player_track_kind { PLAYER_AUDIO, PLAYER_SUBTITLES };
struct player_track {
	char name[96];
	bool on;
};
int player_tracks(struct player *player, enum player_track_kind kind, struct player_track *out,
	int max);
// track i on (subtitles: -1 for none): the sound changes over where it is
void player_set_track(struct player *player, enum player_track_kind kind, int i);
// the subtitles on to draw (subs.h), NULL for none
struct subs;
struct subs *player_subs(struct player *player);

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
// the frame a seek went to not shown yet, still on its way (decoded from the
// keyframe before it)
bool player_seeking(const struct player *player);
// where it is now, seconds
double player_time(const struct player *player);
void player_set_volume(struct player *player, double volume);

// the picture to show now: the newest one due (older ones it skips), NULL
// for nothing new. it's the window's till player_picture_done
struct player_picture *player_due(struct player *player);
void player_picture_done(struct player *player, struct player_picture *picture);

#endif
