#include "usage.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define KEPT 512

static struct {
	char key[PATH_MAX];
	int count;
} picks[KEPT];
static int len;

static void path_of(char *out, size_t size, bool make) {
	const char *state = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
	char dir[PATH_MAX];
	if (state && *state) {
		snprintf(dir, sizeof(dir), "%s/flame", state);
	} else {
		snprintf(dir, sizeof(dir), "%s/.local/state/flame", home ? home : "");
	}
	if (make) {
		char parent[PATH_MAX];
		snprintf(parent, sizeof(parent), "%s", dir);
		char *slash = strrchr(parent, '/');
		if (slash) {
			*slash = '\0';
			mkdir(parent, 0700);
		}
		mkdir(dir, 0700);
	}
	snprintf(out, size, "%s/search", dir);
}

void usage_load(void) {
	char path[PATH_MAX];
	path_of(path, sizeof(path), false);
	FILE *f = fopen(path, "r");
	if (!f) {
		return;
	}
	char line[PATH_MAX + 32];
	len = 0;
	while (len < KEPT && fgets(line, sizeof(line), f)) {
		char *tab = strchr(line, '\t');
		if (!tab) {
			continue;
		}
		*tab = '\0';
		tab[strcspn(tab + 1, "\n") + 1] = '\0';
		picks[len].count = atoi(line);
		snprintf(picks[len].key, sizeof(picks[len].key), "%s", tab + 1);
		len += picks[len].count > 0 && picks[len].key[0];
	}
	fclose(f);
}

static void save(void) {
	char path[PATH_MAX], tmp[PATH_MAX + 8];
	path_of(path, sizeof(path), true);
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		return;
	}
	for (int i = 0; i < len; i++) {
		fprintf(f, "%d\t%s\n", picks[i].count, picks[i].key);
	}
	if (fclose(f) == 0) {
		rename(tmp, path);
	}
}

void usage_add(const char *key) {
	int i = 0;
	while (i < len && strcmp(picks[i].key, key)) {
		i++;
	}
	if (i == len) {
		if (len == KEPT) {
			// full: the least picked one goes
			int least = 0;
			for (int k = 1; k < len; k++) {
				least = picks[k].count < picks[least].count ? k : least;
			}
			i = least;
		} else {
			len++;
		}
		snprintf(picks[i].key, sizeof(picks[i].key), "%s", key);
		picks[i].count = 0;
	}
	picks[i].count++;
	save();
}

int usage_boost(const char *key) {
	for (int i = 0; i < len; i++) {
		if (!strcmp(picks[i].key, key)) {
			return (int)fmin(60 * log2(1 + picks[i].count), 300);
		}
	}
	return 0;
}
