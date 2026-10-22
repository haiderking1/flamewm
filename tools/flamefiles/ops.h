#ifndef FLAMEFILES_OPS_H
#define FLAMEFILES_OPS_H

#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../flameui/flameui.h"

// what flamefiles does to files. the quick ones right away, copying and
// moving on a worker thread with its progress. the trash is the
// freedesktop one (other file managers and apps see the same trash)

// path opened with the user's app for it (xdg-open), not waited on
void ops_open(const char *path);
// a new folder in dir, "untitled folder" (" 2" and on if it's there): its
// name in out. false with error
bool ops_new_folder(const char *dir, char *out, size_t size, const char **error);
// dir/from renamed to dir/to, never over something there
bool ops_rename(const char *dir, const char *from, const char *to, const char **error);

// the trash: files go there, come back to where they were
bool ops_trash(const char *path, const char **error);
// the home trash's folder of trashed files ($XDG_DATA_HOME/Trash/files),
// made when it isn't there
void ops_trash_dir(char *out, size_t size);
// name (in the trash's files) back where it came from
bool ops_put_back(const char *name, const char **error);
// gone for good: path, all of it
bool ops_delete(const char *path, const char **error);

// copying or moving files into a folder, on a thread of its own
struct ops_job {
	struct fui *ui; // woken as it goes
	char **paths;
	size_t len;
	char dest[PATH_MAX];
	bool move;

	pthread_t thread;
	pthread_mutex_t lock;
	// under lock: how far, what's on now, done, why it stopped
	uint64_t total, done_bytes;
	char current[NAME_MAX + 1];
	bool finished, cancel;
	char error[160];
	// the names it made in dest, the first one (to select after)
	char first[NAME_MAX + 1];
	// the thread's own
	char *buf;
	double last_wake;
};

// paths copied (or moved) into dest: what'd go over something there gets
// " 2" (a copy into its own folder: " copy"). NULL when it can't start
struct ops_job *ops_job_start(struct fui *ui, char *const *paths, size_t len, const char *dest,
	bool move);
// it's finished: freed, its thread joined
void ops_job_free(struct ops_job *job);

#endif
