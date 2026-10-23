#ifndef FLAMEBAR_SOUND_H
#define FLAMEBAR_SOUND_H

#include <stdbool.h>
#include <stddef.h>

// the default output and input (the speakers, the mic): their level and
// mute, the devices there are to pick from, through pulseaudio's api
// (pipewire answers it). followed as they change, wake called (from its
// thread) when they do

enum sound_side { SOUND_OUTPUT, SOUND_INPUT };

struct sound_device {
	char id[128]; // the server's name for it
	char name[128]; // the one to show
	bool current;
};

struct sound;

struct sound *sound_start(void (*wake)(void *data), void *data);
void sound_stop(struct sound *sound);

// false while there's none (no sound server yet, no mic)
bool sound_get(struct sound *sound, enum sound_side side, double *volume, bool *muted,
	char *name, size_t name_size);
void sound_set_volume(struct sound *sound, enum sound_side side, double volume);
void sound_set_muted(struct sound *sound, enum sound_side side, bool muted);
// the devices there are (the mic side: real ones, not what an output plays
// back), how many
int sound_devices(struct sound *sound, enum sound_side side, struct sound_device *out, int max);
// that one the default from now on
void sound_set_device(struct sound *sound, enum sound_side side, const char *id);

#endif
