#ifndef FLAME_ANIM_ANIM_H
#define FLAME_ANIM_ANIM_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct flame_server;

// animations: tweens along a bezier curve, stepped once a frame to where
// they'll be when that frame is on screen. a tween starts with the first
// frame that can show it, so none of it gets skipped

// cubic-bezier(x1, y1, x2, y2), like css
struct flame_curve {
	double x1, y1, x2, y2;
};

// how far along a curve is at time t, 0..1. y can overshoot for bouncy ones
double curve_ease(const struct flame_curve *curve, double t);

struct flame_anim {
	struct wl_list link; // flame_anims.running
	int64_t start; // ns, 0 until the first frame it's in
	int64_t duration; // ns
	struct flame_curve curve;
	// eased: along the curve, t: along the time. t 1 is the last step
	void (*step)(struct flame_anim *anim, double eased, double t);
	// it ran to the end or got cut short. may free anim
	void (*done)(struct flame_anim *anim, bool finished);
	void *data;
	bool running;
};

struct flame_anims {
	struct flame_server *server;
	struct wl_list running; // flame_anim.link
	// no output drawing frames (screens off, none there): they step
	// themselves from here, so nothing hangs half way
	struct wl_event_source *watchdog;
	int64_t last_tick; // ns
};

void anims_init(struct flame_anims *anims, struct flame_server *server);
void anims_finish(struct flame_anims *anims);

// runs anim from its next frame on. one already running starts over. a
// duration of 0 jumps to the end right away
void anim_start(struct flame_anims *anims, struct flame_anim *anim, int duration_ms,
	const struct flame_curve *curve);
// stops it where it is, done hears finished = false
void anim_cancel(struct flame_anim *anim);
// to the end now: the last step, then done with finished = true
void anim_finish(struct flame_anim *anim);
bool anim_running(const struct flame_anim *anim);

// a frame is about to be built for target (ns): everything steps there
void anims_tick(struct flame_anims *anims, int64_t target);

// a + (b - a) * t
double lerp(double a, double b, double t);

#endif
