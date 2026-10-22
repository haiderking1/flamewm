#ifndef FLAME_GAME_PACER_H
#define FLAME_GAME_PACER_H

#include <stdbool.h>
#include <stdint.h>

// a game's frames on screen at an even pace, the fastest one it can hold.
// flame lets the game start each frame (gives it a buffer back) just in
// time for the frame to be ready at its slot, and shows it on the slot: the
// line stays flat and what's on screen is the freshest input. the pace
// follows the game, gliding: slowly faster when it has room, quick to back
// off when it doesn't, so nothing on screen jumps. after a hitch the frames
// that pile up keep the pace instead of rushing out. pure logic, times in
// ns, see game/pacer.c. what lets the game go and what shows the frames is
// the X server's business (xshell)

// when the fullscreen game gets paced: never, while the display runs vrr
// (it follows the frames, any pace shows as it is), or always (a fixed
// refresh shows a pace that isn't a divisor of it unevenly)
enum pacer_mode {
	PACER_OFF,
	PACER_VRR,
	PACER_ALWAYS,
};

// NULL is the default, vrr. false for anything else than off/vrr/always
bool pacer_mode_parse(const char *value, enum pacer_mode *mode);

// frames remembered for what the game can do, about a second of them
#define PACER_SAMPLES 96

struct pacer {
	int64_t fastest; // the shortest interval the display does, its top refresh
	int64_t slowest; // slower than this isn't worth pacing, the game shows as it comes
	int64_t fixed; // a pace from the config, 0: follow the game
	int64_t interval; // the pace now: ns between frames on screen

	// per frame: let go -> picture ready on the gpu (lead), and the busier
	// of its cpu and gpu parts (cost: how often it could make one)
	int64_t lead[PACER_SAMPLES], cost[PACER_SAMPLES];
	int len, at;

	int64_t released; // when the game was let go for the frame it's on, 0 none
	int64_t last_ready; // when the last frame's gpu work was done
	int64_t last_shown; // the last frame's slot, 0 before the first
};

// fastest: the display's top refresh. fixed: a pace to keep (a cap), 0 for
// the game's own
void pacer_init(struct pacer *pacer, int64_t fastest, int64_t fixed);
// the display or the config changed
void pacer_set_limits(struct pacer *pacer, int64_t fastest, int64_t fixed);
// the game is let go for its next frame at t
void pacer_released(struct pacer *pacer, int64_t t);
// a frame came: presented at presented, its gpu work done at ready
void pacer_frame(struct pacer *pacer, int64_t presented, int64_t ready);
// when a frame ready at ready goes on screen: on its slot, or right away
// when it's late for it (the slots start over from there)
int64_t pacer_slot(const struct pacer *pacer, int64_t ready);
// it went on screen at slot
void pacer_shown(struct pacer *pacer, int64_t slot);
// when to let the game go for the next frame, so it's ready for the slot
// after the last one. 0 before the first frame: right away
int64_t pacer_release_at(const struct pacer *pacer);
// frames per second it's paced at, for the log and flamectl state
double pacer_fps(const struct pacer *pacer);

#endif
