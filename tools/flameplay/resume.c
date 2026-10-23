#define _GNU_SOURCE // getline
#include "resume.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// a line each, the newest first: seconds, a tab, the file's full path
#define KEPT 500

// $XDG_STATE_HOME/flame/flameplay-<name>
static void state_path(const char *name, char *out, size_t size) {
	const char *state = getenv("XDG_STATE_HOME");
	const char *home = getenv("HOME");
	if (state && *state == '/') {
		snprintf(out, size, "%s/flame/flameplay-%s", state, name);
	} else {
		snprintf(out, size, "%s/.local/state/flame/flameplay-%s", home ? home : "/tmp", name);
	}
}

static void list_path(char *out, size_t size) {
	state_path("positions", out, size);
}

// the list's folder and the ones it's in
static void make_dirs(char *list) {
	for (char *c = strchr(list + 1, '/'); c; c = strchr(c + 1, '/')) {
		*c = '\0';
		mkdir(list, 0755);
		*c = '/';
	}
}

// the same file however it was named (relative, through links)
static bool full_path(const char *path, char *out) {
	return realpath(path, out) && !strchr(out, '\n');
}

double resume_get(const char *path) {
	char full[PATH_MAX], list[PATH_MAX];
	if (!full_path(path, full)) {
		return 0;
	}
	list_path(list, sizeof(list));
	FILE *f = fopen(list, "re");
	char *line = NULL;
	size_t cap = 0;
	double at = 0;
	while (f && getline(&line, &cap, f) > 0) {
		char *tab = strchr(line, '\t');
		if (!tab) {
			continue;
		}
		tab[strcspn(tab, "\n")] = '\0';
		if (!strcmp(tab + 1, full)) {
			at = strtod(line, NULL);
			break;
		}
	}
	free(line);
	if (f) {
		fclose(f);
	}
	return isfinite(at) && at > 0 ? at : 0;
}

void resume_put(const char *path, double t, double duration, bool video) {
	char full[PATH_MAX], list[PATH_MAX], tmp[PATH_MAX + 8];
	if (!full_path(path, full)) {
		return;
	}
	// worth going back to: a video a minute long or more (only sound: twenty,
	// an audiobook, not a song), past its start, not in its last bit (the
	// credits)
	bool keep = duration >= (video ? 60 : 20 * 60) && t >= 10
		&& t < duration - fmax(10, duration * 0.05);
	list_path(list, sizeof(list));
	FILE *in = fopen(list, "re");
	if (!keep && !in) {
		return;
	}
	make_dirs(list);
	snprintf(tmp, sizeof(tmp), "%s.new", list);
	FILE *out = fopen(tmp, "we");
	if (!out) {
		if (in) {
			fclose(in);
		}
		return;
	}
	int kept = 0;
	if (keep) {
		fprintf(out, "%.2f\t%s\n", t, full);
		kept++;
	}
	// the rest as they were, this one's old line dropped
	char *line = NULL;
	size_t cap = 0;
	while (in && kept < KEPT && getline(&line, &cap, in) > 0) {
		char *tab = strchr(line, '\t');
		size_t len = strlen(full);
		if (!tab || (!strncmp(tab + 1, full, len) && (tab[1 + len] == '\n' || !tab[1 + len]))) {
			continue;
		}
		fputs(line, out);
		kept++;
	}
	free(line);
	if (in) {
		fclose(in);
	}
	// whole or not at all: a crash halfway leaves the old list
	if (fclose(out) == 0) {
		rename(tmp, list);
	} else {
		remove(tmp);
	}
}

bool resume_volume(double *volume, double *unmuted) {
	char path[PATH_MAX];
	state_path("volume", path, sizeof(path));
	FILE *f = fopen(path, "re");
	double v, u;
	bool got = f && fscanf(f, "%lf %lf", &v, &u) == 2 && isfinite(v) && isfinite(u);
	if (f) {
		fclose(f);
	}
	if (got) {
		*volume = fmax(0, fmin(v, 1));
		*unmuted = fmax(0, fmin(u, 1));
	}
	return got;
}

void resume_put_volume(double volume, double unmuted) {
	char path[PATH_MAX], tmp[PATH_MAX + 8];
	state_path("volume", path, sizeof(path));
	make_dirs(path);
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	FILE *f = fopen(tmp, "we");
	if (!f) {
		return;
	}
	fprintf(f, "%.3f %.3f\n", volume, unmuted);
	if (fclose(f) == 0) {
		rename(tmp, path);
	} else {
		remove(tmp);
	}
}
