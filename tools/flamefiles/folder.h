#ifndef FLAMEFILES_FOLDER_H
#define FLAMEFILES_FOLDER_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <time.h>

// a folder's files as the window shows them: what each is, how big, when it
// changed, sorted the way the user asked. inotify says when it changes

enum file_kind {
	KIND_FOLDER,
	KIND_IMAGE,
	KIND_VIDEO,
	KIND_AUDIO,
	KIND_ARCHIVE,
	KIND_CODE,
	KIND_TEXT,
	KIND_PDF,
	KIND_DOCUMENT,
	KIND_PROGRAM,
	KIND_OTHER,
};

struct file {
	char *name;
	enum file_kind kind;
	bool hidden, link;
	off_t size; // bytes, folders: how many things in it (-1 unknown)
	time_t mtime;
	char kind_name[32]; // "PNG image", "Folder"
};

enum sort_key {
	SORT_NAME,
	SORT_DATE,
	SORT_SIZE,
	SORT_KIND,
};

struct folder {
	char path[PATH_MAX]; // no trailing slash but for /
	struct file *files;
	size_t len;
	int inotify; // -1 for none
	int watch;
};

void folder_init(struct folder *folder);
void folder_finish(struct folder *folder);
// path listed (hidden files too, the window filters), false and nothing
// changed when it can't be read. the watch moves with it
bool folder_list(struct folder *folder, const char *path);
// folders first, then by key
void folder_sort(struct folder *folder, enum sort_key key, bool descending);
// inotify's events read: true when the folder changed
bool folder_changed(struct folder *folder);

// what a name like this is, by its extension
enum file_kind kind_of_name(const char *name);
// human: "4.2 MB", "12 items"
void format_size(const struct file *file, char *out, size_t size);
// Finder's: "Today at 14:03", "Yesterday at 09:10", "22 Oct 2026 at 14:03"
void format_date(time_t when, char *out, size_t size);

#endif
