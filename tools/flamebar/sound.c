#include "sound.h"

#include <math.h>
#include <pulse/pulseaudio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// pulse's threaded loop: its callbacks on its thread, what they learn kept
// under its lock. the sink and source followed are the server's defaults,
// whatever they change to

#define DEVICES 16

struct side {
	bool have;
	double volume;
	bool muted;
	char id[128], name[128];
	pa_cvolume channels; // its last volume, the shape a new one takes
	struct sound_device devices[DEVICES];
	int count;
	// a list coming in, the shown one swapped for it when it's all there
	struct sound_device next[DEVICES];
	int next_count;
};

struct sound {
	pa_threaded_mainloop *loop;
	pa_context *context;
	void (*wake)(void *data);
	void *wake_data;
	// under the loop's lock
	struct side sides[2];
	bool reconnect;
};

static const char *const defaults[] = {"@DEFAULT_SINK@", "@DEFAULT_SOURCE@"};

static void connect_context(struct sound *s);

static void took(struct sound *s, struct side *side, const char *id, const char *name,
		const pa_cvolume *volume, int mute) {
	side->have = true;
	side->volume = (double)pa_cvolume_avg(volume) / PA_VOLUME_NORM;
	side->muted = mute;
	side->channels = *volume;
	snprintf(side->id, sizeof(side->id), "%s", id ? id : "");
	snprintf(side->name, sizeof(side->name), "%s", name ? name : "");
	s->wake(s->wake_data);
}

static void sink_info(pa_context *c, const pa_sink_info *info, int eol, void *data) {
	if (!eol && info) {
		struct sound *s = data;
		took(s, &s->sides[SOUND_OUTPUT], info->name, info->description, &info->volume,
			info->mute);
	}
}

static void source_info(pa_context *c, const pa_source_info *info, int eol, void *data) {
	struct sound *s = data;
	if (!eol && info) {
		took(s, &s->sides[SOUND_INPUT], info->name, info->description, &info->volume,
			info->mute);
	} else if (eol < 0) {
		s->sides[SOUND_INPUT].have = false; // there's no mic
		s->wake(s->wake_data);
	}
}

static void listed(struct sound *s, struct side *side, const char *id, const char *name,
		int eol) {
	if (eol) {
		memcpy(side->devices, side->next, sizeof(side->next));
		side->count = side->next_count;
		side->next_count = 0;
		s->wake(s->wake_data);
		return;
	}
	if (side->next_count < DEVICES) {
		struct sound_device *d = &side->next[side->next_count++];
		snprintf(d->id, sizeof(d->id), "%s", id ? id : "");
		snprintf(d->name, sizeof(d->name), "%s", name && *name ? name : id ? id : "");
	}
}

static void sink_list(pa_context *c, const pa_sink_info *info, int eol, void *data) {
	struct sound *s = data;
	listed(s, &s->sides[SOUND_OUTPUT], info ? info->name : NULL,
		info ? info->description : NULL, eol || !info);
}

static void source_list(pa_context *c, const pa_source_info *info, int eol, void *data) {
	struct sound *s = data;
	// an output's monitor (what it plays, as an input) is no mic
	if (!eol && info && info->monitor_of_sink != PA_INVALID_INDEX) {
		return;
	}
	listed(s, &s->sides[SOUND_INPUT], info ? info->name : NULL,
		info ? info->description : NULL, eol || !info);
}

static void done(pa_operation *op) {
	if (op) {
		pa_operation_unref(op);
	}
}

// the defaults and what there is, looked at again
static void refresh(struct sound *s) {
	done(pa_context_get_sink_info_by_name(s->context, defaults[SOUND_OUTPUT], sink_info, s));
	done(pa_context_get_source_info_by_name(s->context, defaults[SOUND_INPUT], source_info, s));
	s->sides[SOUND_OUTPUT].next_count = s->sides[SOUND_INPUT].next_count = 0;
	done(pa_context_get_sink_info_list(s->context, sink_list, s));
	done(pa_context_get_source_info_list(s->context, source_list, s));
}

static void changed(pa_context *c, pa_subscription_event_type_t type, uint32_t index,
		void *data) {
	refresh(data);
}

static void retry(pa_mainloop_api *api, pa_time_event *event, const struct timeval *tv,
		void *data) {
	api->time_free(event);
	connect_context(data);
}

static void state(pa_context *c, void *data) {
	struct sound *s = data;
	switch (pa_context_get_state(c)) {
	case PA_CONTEXT_READY:
		pa_context_set_subscribe_callback(c, changed, s);
		done(pa_context_subscribe(c, PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE
			| PA_SUBSCRIPTION_MASK_SERVER, NULL, NULL));
		refresh(s);
		break;
	case PA_CONTEXT_FAILED:
	case PA_CONTEXT_TERMINATED:
		// the server went (restarted): nothing till it's back, tried again
		// every couple of seconds
		s->sides[SOUND_OUTPUT].have = s->sides[SOUND_INPUT].have = false;
		s->wake(s->wake_data);
		if (s->reconnect) {
			struct timeval tv;
			pa_mainloop_api *api = pa_threaded_mainloop_get_api(s->loop);
			api->time_new(api, pa_timeval_add(pa_gettimeofday(&tv), 2000000), retry, s);
		}
		break;
	default:
		break;
	}
}

static void connect_context(struct sound *s) {
	if (s->context) {
		pa_context_set_state_callback(s->context, NULL, NULL);
		pa_context_disconnect(s->context);
		pa_context_unref(s->context);
	}
	s->context = pa_context_new(pa_threaded_mainloop_get_api(s->loop), "flamebar");
	if (!s->context) {
		return;
	}
	pa_context_set_state_callback(s->context, state, s);
	pa_context_connect(s->context, NULL, PA_CONTEXT_NOFAIL, NULL);
}

struct sound *sound_start(void (*wake)(void *data), void *data) {
	struct sound *s = calloc(1, sizeof(*s));
	if (!s) {
		return NULL;
	}
	s->wake = wake;
	s->wake_data = data;
	s->reconnect = true;
	s->loop = pa_threaded_mainloop_new();
	if (!s->loop) {
		free(s);
		return NULL;
	}
	pa_threaded_mainloop_lock(s->loop);
	connect_context(s);
	pa_threaded_mainloop_unlock(s->loop);
	if (pa_threaded_mainloop_start(s->loop) < 0) {
		sound_stop(s);
		return NULL;
	}
	return s;
}

void sound_stop(struct sound *s) {
	if (!s) {
		return;
	}
	pa_threaded_mainloop_lock(s->loop);
	s->reconnect = false;
	if (s->context) {
		pa_context_set_state_callback(s->context, NULL, NULL);
		pa_context_disconnect(s->context);
		pa_context_unref(s->context);
		s->context = NULL;
	}
	pa_threaded_mainloop_unlock(s->loop);
	pa_threaded_mainloop_stop(s->loop);
	pa_threaded_mainloop_free(s->loop);
	free(s);
}

bool sound_get(struct sound *s, enum sound_side which, double *volume, bool *muted,
		char *name, size_t name_size) {
	if (!s) {
		return false;
	}
	pa_threaded_mainloop_lock(s->loop);
	struct side *side = &s->sides[which];
	bool have = side->have;
	*volume = side->volume;
	*muted = side->muted;
	snprintf(name, name_size, "%s", side->name);
	pa_threaded_mainloop_unlock(s->loop);
	return have;
}

void sound_set_volume(struct sound *s, enum sound_side which, double volume) {
	if (!s) {
		return;
	}
	pa_threaded_mainloop_lock(s->loop);
	struct side *side = &s->sides[which];
	if (side->have && s->context) {
		pa_cvolume v = side->channels;
		pa_volume_t level = (pa_volume_t)lround(fmax(0, fmin(volume, 1)) * PA_VOLUME_NORM);
		pa_cvolume_set(&v, v.channels ? v.channels : 2, level);
		done(which == SOUND_OUTPUT
			? pa_context_set_sink_volume_by_name(s->context, defaults[which], &v, NULL, NULL)
			: pa_context_set_source_volume_by_name(s->context, defaults[which], &v, NULL, NULL));
		side->volume = volume; // shown now, not when the server says so
	}
	pa_threaded_mainloop_unlock(s->loop);
}

void sound_set_muted(struct sound *s, enum sound_side which, bool muted) {
	if (!s) {
		return;
	}
	pa_threaded_mainloop_lock(s->loop);
	struct side *side = &s->sides[which];
	if (side->have && s->context) {
		done(which == SOUND_OUTPUT
			? pa_context_set_sink_mute_by_name(s->context, defaults[which], muted, NULL, NULL)
			: pa_context_set_source_mute_by_name(s->context, defaults[which], muted, NULL, NULL));
		side->muted = muted;
	}
	pa_threaded_mainloop_unlock(s->loop);
}

int sound_devices(struct sound *s, enum sound_side which, struct sound_device *out, int max) {
	if (!s) {
		return 0;
	}
	pa_threaded_mainloop_lock(s->loop);
	struct side *side = &s->sides[which];
	int n = side->count < max ? side->count : max;
	for (int i = 0; i < n; i++) {
		out[i] = side->devices[i];
		out[i].current = !strcmp(out[i].id, side->id);
	}
	pa_threaded_mainloop_unlock(s->loop);
	return n;
}

void sound_set_device(struct sound *s, enum sound_side which, const char *id) {
	if (!s) {
		return;
	}
	pa_threaded_mainloop_lock(s->loop);
	if (s->context) {
		done(which == SOUND_OUTPUT ? pa_context_set_default_sink(s->context, id, NULL, NULL)
			: pa_context_set_default_source(s->context, id, NULL, NULL));
	}
	pa_threaded_mainloop_unlock(s->loop);
}
