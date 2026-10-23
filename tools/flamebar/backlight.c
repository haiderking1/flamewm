#include "backlight.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DIR_PATH "/sys/class/backlight"

struct backlight {
	sd_bus *bus;
	char name[256];
	long max;
	double level; // what it was set to last, read again after a while
	double set_at;
};

static double now_seconds(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static long read_number(const char *name, const char *file) {
	char path[512];
	snprintf(path, sizeof(path), DIR_PATH "/%s/%s", name, file);
	FILE *f = fopen(path, "r");
	long value = -1;
	if (f) {
		if (fscanf(f, "%ld", &value) != 1) {
			value = -1;
		}
		fclose(f);
	}
	return value;
}

// the panel's own (firmware) over the platform's over a gpu's raw one,
// the way systemd picks
static int rank(const char *name) {
	char path[512], type[32] = "";
	snprintf(path, sizeof(path), DIR_PATH "/%s/type", name);
	FILE *f = fopen(path, "r");
	if (f) {
		if (fscanf(f, "%31s", type) != 1) {
			type[0] = '\0';
		}
		fclose(f);
	}
	return !strcmp(type, "firmware") ? 3 : !strcmp(type, "platform") ? 2 : 1;
}

struct backlight *backlight_start(sd_bus *system) {
	DIR *dir = opendir(DIR_PATH);
	if (!system || !dir) {
		if (dir) {
			closedir(dir);
		}
		return NULL;
	}
	char best[256] = "";
	int best_rank = 0;
	struct dirent *e;
	while ((e = readdir(dir))) {
		if (e->d_name[0] == '.' || read_number(e->d_name, "max_brightness") <= 0) {
			continue;
		}
		int r = rank(e->d_name);
		if (r > best_rank) {
			best_rank = r;
			snprintf(best, sizeof(best), "%s", e->d_name);
		}
	}
	closedir(dir);
	if (!*best) {
		return NULL;
	}
	struct backlight *b = calloc(1, sizeof(*b));
	if (!b) {
		return NULL;
	}
	b->bus = sd_bus_ref(system);
	snprintf(b->name, sizeof(b->name), "%s", best);
	b->max = read_number(best, "max_brightness");
	b->level = -1;
	return b;
}

void backlight_stop(struct backlight *b) {
	if (b) {
		sd_bus_unref(b->bus);
		free(b);
	}
}

double backlight_get(struct backlight *b) {
	if (!b) {
		return 0;
	}
	// what was just set (logind's still at it), else what it is
	if (b->level >= 0 && now_seconds() - b->set_at < 2) {
		return b->level;
	}
	long value = read_number(b->name, "brightness");
	return value >= 0 ? (double)value / (double)b->max : 0;
}

void backlight_set(struct backlight *b, double level) {
	if (!b) {
		return;
	}
	// never quite off: a screen at 0 is a black one, nothing to turn it up with
	level = fmax(0.01, fmin(level, 1));
	uint32_t value = (uint32_t)fmax(1, lround(level * (double)b->max));
	sd_bus_call_method_async(b->bus, NULL, "org.freedesktop.login1",
		"/org/freedesktop/login1/session/auto", "org.freedesktop.login1.Session",
		"SetBrightness", NULL, NULL, "ssu", "backlight", b->name, value);
	b->level = level;
	b->set_at = now_seconds();
}
