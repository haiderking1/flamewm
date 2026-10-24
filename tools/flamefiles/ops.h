#ifndef FLAMEFILES_OPS_H
#define FLAMEFILES_OPS_H

#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "../flameui/flameui.h"

// what flamefiles does to files. the quick ones right away, copying and
// moving on a worker thread with its progress. the trash is the
// freedesktop one (other file managers and apps see the same trash)

// path opened with the user's app for it (xdg-open), not waited on
void ops_open(const char *path);
// with one of flame's own apps (flameplay): next to flamefiles, else on
// $PATH, else the user's app for it after all
void ops_open_with(const char *app, const char *path);
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

// what to do with one that's going where something by its name already
// is: asked (the job waits for it), the one there to the trash and this in
// its place, both kept (this one " 2"), this one left out
enum ops_clash_answer { OPS_ASK, OPS_REPLACE, OPS_KEEP_BOTH, OPS_SKIP };

// a name that's there already, what it and the one coming are
struct ops_clash {
	char name[NAME_MAX + 1];
	bool folder, new_folder;
	long long size, new_size; // bytes, a folder's: how many things in it
	time_t mtime, new_mtime;
	size_t others; // more clashes after this one in the job
};

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
	// a clash it's waiting on (asking), what it is, the answer (and whether
	// it goes for the rest too), said to it with ops_job_answer
	bool asking;
	struct ops_clash clash;
	enum ops_clash_answer answer;
	bool answer_all;
	pthread_cond_t answered;
	// the thread's own
	char *buf;
	double last_wake;
};

// paths copied (or moved) into dest. what'd go over something there is
// asked about (asking, the ui woken), a copy into its own folder gets
// " copy". NULL when it can't start
struct ops_job *ops_job_start(struct fui *ui, char *const *paths, size_t len, const char *dest,
	bool move);
// the clash asked about answered, all: the rest of them too
void ops_job_answer(struct ops_job *job, enum ops_clash_answer answer, bool all);
// stopped where it is (a clash asked about too)
void ops_job_cancel(struct ops_job *job);
// it's finished: freed, its thread joined
void ops_job_free(struct ops_job *job);

#endif
