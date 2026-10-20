#include "anim/anim.h"

#include <math.h>

#include "core/server.h"

// ---- curves ----

static double bezier(double a, double b, double t) {
	// a cubic from 0 to 1 with control points a and b
	double u = 1 - t;
	return 3 * u * u * t * a + 3 * u * t * t * b + t * t * t;
}

static double bezier_slope(double a, double b, double t) {
	double u = 1 - t;
	return 3 * u * u * a + 6 * u * t * (b - a) + 3 * t * t * (1 - b);
}

double curve_ease(const struct flame_curve *curve, double t) {
	if (t <= 0) {
		return 0;
	}
	if (t >= 1) {
		return 1;
	}
	// the curve's x is time: find where it reaches t, newton first, halving
	// when the slope is too flat to trust
	double s = t;
	for (int i = 0; i < 8; i++) {
		double x = bezier(curve->x1, curve->x2, s) - t;
		if (fabs(x) < 1e-6) {
			return bezier(curve->y1, curve->y2, s);
		}
		double slope = bezier_slope(curve->x1, curve->x2, s);
		if (fabs(slope) < 1e-6) {
			break;
		}
		s -= x / slope;
	}
	double lo = 0, hi = 1;
	s = t;
	for (int i = 0; i < 32; i++) {
		double x = bezier(curve->x1, curve->x2, s);
		if (fabs(x - t) < 1e-6) {
			break;
		}
		if (x < t) {
			lo = s;
		} else {
			hi = s;
		}
		s = (lo + hi) / 2;
	}
	return bezier(curve->y1, curve->y2, s);
}

double lerp(double a, double b, double t) {
	return a + (b - a) * t;
}

// ---- running them ----

void anims_init(struct flame_anims *anims, struct flame_server *server) {
	anims->server = server;
	wl_list_init(&anims->running);
}

void anims_finish(struct flame_anims *anims) {
	while (!wl_list_empty(&anims->running)) {
		struct flame_anim *anim = wl_container_of(anims->running.next, anim, link);
		anim_cancel(anim);
	}
}

bool anim_running(const struct flame_anim *anim) {
	return anim->running;
}

static void stop(struct flame_anim *anim) {
	wl_list_remove(&anim->link);
	wl_list_init(&anim->link);
	anim->running = false;
}

void anim_cancel(struct flame_anim *anim) {
	if (!anim->running) {
		return;
	}
	stop(anim);
	if (anim->done) {
		anim->done(anim, false);
	}
}

void anim_finish(struct flame_anim *anim) {
	if (!anim->running) {
		return;
	}
	stop(anim);
	anim->step(anim, 1, 1);
	if (anim->done) {
		anim->done(anim, true);
	}
}

void anim_start(struct flame_anims *anims, struct flame_anim *anim, int duration_ms,
		const struct flame_curve *curve) {
	if (anim->running) {
		stop(anim);
	}
	anim->start = 0;
	anim->duration = (int64_t)duration_ms * 1000000;
	anim->curve = *curve;
	anim->running = true;
	wl_list_insert(anims->running.prev, &anim->link);
	if (duration_ms <= 0) {
		anim_finish(anim);
		return;
	}
	// its first step is the next frame's: show where it starts
	anim->step(anim, 0, 0);
	outputs_schedule_frame(&anims->server->outputs);
}

void anims_tick(struct flame_anims *anims, int64_t target) {
	if (wl_list_empty(&anims->running)) {
		return;
	}
	// steps can start, cancel and free animations: one at a time, each
	// taken off the list before its step
	struct wl_list due;
	wl_list_init(&due);
	wl_list_insert_list(&due, &anims->running);
	wl_list_init(&anims->running);
	while (!wl_list_empty(&due)) {
		struct flame_anim *anim = wl_container_of(due.next, anim, link);
		wl_list_remove(&anim->link);
		wl_list_insert(anims->running.prev, &anim->link);
		if (!anim->start) {
			anim->start = target;
		}
		double t = (double)(target - anim->start) / (double)anim->duration;
		if (t >= 1) {
			anim_finish(anim);
			continue;
		}
		anim->step(anim, curve_ease(&anim->curve, t), t);
	}
	if (!wl_list_empty(&anims->running)) {
		outputs_schedule_frame(&anims->server->outputs);
	}
}
