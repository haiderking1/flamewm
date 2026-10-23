#include "files.h"

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "match.h"
#include "usage.h"

#define MOST 300000 // files, past that a home's not indexed further
#define DEEPEST 10
#define STALE 120 // seconds an index is good for

struct entry {
	uint32_t path; // in text: the whole path
	uint32_t name; // in text: its name, folded
	uint8_t depth;
	bool dir;
	time_t mtime;
};

struct index {
	struct entry *entries;
	size_t len, cap;
	char *text;
	size_t text_len, text_cap;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct index *now; // under lock
static time_t made; // under lock, when now was
static bool making; // under lock
static pthread_t thread;
static bool thread_up;
static atomic_bool stopping; // quitting: a walk under way ends where it is

// places with thousands of files of their own, never what's looked for
static bool skipped(const char *name) {
	static const char *const names[] = {
		"node_modules", "__pycache__", "site-packages", "venv", "target", "go", "snap",
		"CMakeFiles", "vendor",
	};
	if (name[0] == '.') {
		return true; // hidden, and what's under it
	}
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (!strcmp(name, names[i])) {
			return true;
		}
	}
	return false;
}

static uint32_t add_text(struct index *x, const char *s) {
	size_t n = strlen(s) + 1;
	if (x->text_len + n > x->text_cap) {
		size_t cap = x->text_cap ? x->text_cap * 2 : 1 << 20;
		while (cap < x->text_len + n) {
			cap *= 2;
		}
		char *grown = realloc(x->text, cap);
		if (!grown) {
			return UINT32_MAX;
		}
		x->text = grown;
		x->text_cap = cap;
	}
	uint32_t at = (uint32_t)x->text_len;
	memcpy(x->text + at, s, n);
	x->text_len += n;
	return at;
}

static bool add(struct index *x, const char *path, const char *name, int depth, bool dir,
		time_t mtime) {
	if (x->len == x->cap) {
		size_t cap = x->cap ? x->cap * 2 : 4096;
		struct entry *grown = realloc(x->entries, cap * sizeof(*grown));
		if (!grown) {
			return false;
		}
		x->entries = grown;
		x->cap = cap;
	}
	char folded[512];
	fold(name, folded, sizeof(folded));
	uint32_t p = add_text(x, path), f = add_text(x, folded);
	if (p == UINT32_MAX || f == UINT32_MAX) {
		return false;
	}
	x->entries[x->len++] = (struct entry){p, f, (uint8_t)depth, dir, mtime};
	return true;
}

static void free_index(struct index *x) {
	if (x) {
		free(x->entries);
		free(x->text);
		free(x);
	}
}

// home, a folder at a time (the shallow ones first: a home's own folders
// in before what's deep in a project)
static struct index *walk(void) {
	const char *home = getenv("HOME");
	struct index *x = calloc(1, sizeof(*x));
	if (!x || !home) {
		free(x);
		return NULL;
	}
	// the folders still to read: offsets into x->text, with their depth
	struct todo {
		uint32_t path;
		int depth;
	} *queue = NULL;
	size_t head = 0, tail = 0, cap = 0;
	uint32_t root = add_text(x, home);
	queue = malloc(sizeof(*queue) * (cap = 1024));
	if (!queue || root == UINT32_MAX) {
		free(queue);
		free_index(x);
		return NULL;
	}
	queue[tail++] = (struct todo){root, 0};
	while (head < tail && x->len < MOST && !atomic_load(&stopping)) {
		struct todo t = queue[head++];
		char dir_path[PATH_MAX];
		snprintf(dir_path, sizeof(dir_path), "%s", x->text + t.path);
		DIR *dir = opendir(dir_path);
		if (!dir) {
			continue;
		}
		struct dirent *e;
		while ((e = readdir(dir)) && x->len < MOST) {
			if (skipped(e->d_name) || e->d_type == DT_LNK) {
				continue; // a link can lead back up: not followed
			}
			char path[PATH_MAX];
			if (snprintf(path, sizeof(path), "%s/%s", dir_path, e->d_name) >= (int)sizeof(path)) {
				continue;
			}
			struct stat st;
			if (lstat(path, &st) < 0) {
				continue;
			}
			bool is_dir = S_ISDIR(st.st_mode);
			if (!is_dir && !S_ISREG(st.st_mode)) {
				continue;
			}
			if (!add(x, path, e->d_name, t.depth + 1, is_dir, st.st_mtime)) {
				break;
			}
			if (is_dir && t.depth + 1 < DEEPEST) {
				if (tail == cap) {
					struct todo *grown = realloc(queue, sizeof(*queue) * (cap *= 2));
					if (!grown) {
						continue;
					}
					queue = grown;
				}
				queue[tail++] = (struct todo){x->entries[x->len - 1].path, t.depth + 1};
			}
		}
		closedir(dir);
	}
	free(queue);
	return x;
}

static void *indexer(void *data) {
	struct index *x = walk();
	pthread_mutex_lock(&lock);
	if (x) {
		free_index(now);
		now = x;
		made = time(NULL);
	}
	making = false;
	pthread_mutex_unlock(&lock);
	return NULL;
}

void files_refresh(void) {
	pthread_mutex_lock(&lock);
	bool go = !making && (!now || time(NULL) - made > STALE);
	if (go) {
		making = true;
	}
	pthread_mutex_unlock(&lock);
	if (!go) {
		return;
	}
	if (thread_up) {
		pthread_join(thread, NULL); // the last one's done (making was false)
		thread_up = false;
	}
	thread_up = !pthread_create(&thread, NULL, indexer, NULL);
	if (!thread_up) {
		pthread_mutex_lock(&lock);
		making = false;
		pthread_mutex_unlock(&lock);
	}
}

void files_start(void) {
	files_refresh();
}

void files_stop(void) {
	atomic_store(&stopping, true);
	if (thread_up) {
		pthread_join(thread, NULL);
		thread_up = false;
	}
	pthread_mutex_lock(&lock);
	free_index(now);
	now = NULL;
	pthread_mutex_unlock(&lock);
}

int files_search(const char *query, struct file_hit *out, int max) {
	int found = 0;
	time_t today = time(NULL);
	pthread_mutex_lock(&lock);
	for (size_t i = 0; now && i < now->len; i++) {
		const struct entry *e = &now->entries[i];
		int score = match(query, now->text + e->name, false);
		if (score <= 0) {
			continue;
		}
		// a folder, one near the top of home, one changed lately: first
		score += (e->dir ? 40 : 0) - 25 * e->depth;
		double days = difftime(today, e->mtime) / 86400;
		score += days < 2 ? 80 : days < 14 ? 50 : days < 90 ? 20 : 0;
		const char *path = now->text + e->path;
		score += usage_boost(path);
		int at = found < max ? found : max - 1;
		if (found == max && out[at].score >= score) {
			continue;
		}
		while (at > 0 && out[at - 1].score < score) {
			out[at] = out[at - 1];
			at--;
		}
		snprintf(out[at].path, sizeof(out[at].path), "%s", path);
		out[at].dir = e->dir;
		out[at].score = score;
		found += found < max;
	}
	pthread_mutex_unlock(&lock);
	return found;
}
