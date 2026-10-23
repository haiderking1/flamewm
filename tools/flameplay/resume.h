#ifndef FLAMEPLAY_RESUME_H
#define FLAMEPLAY_RESUME_H

#include <stdbool.h>

// where each video was left, and the sound's level, kept from one run to
// the next ($XDG_STATE_HOME/flame/flameplay-positions, -volume): opened
// again, it goes on from there as loud as it was

// where path was left, 0 for the start
double resume_get(const char *path);
// path left at t of duration (a video, or only sound): kept, or forgotten
// when there's nothing to go back to (short, barely started, watched to the
// end)
void resume_put(const char *path, double t, double duration, bool video);
// the level it was left at (0..1, 0: muted) and the one a mute goes back
// to, false when there's none kept
bool resume_volume(double *volume, double *unmuted);
void resume_put_volume(double volume, double unmuted);

#endif
