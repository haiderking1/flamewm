#ifndef FLAMESEARCH_FILES_H
#define FLAMESEARCH_FILES_H

#include <limits.h>
#include <stdbool.h>

// the files in the home folder, by name: indexed on a thread of its own
// (hidden ones and what's under them left out, and the places code and
// programs keep thousands of their own), again when it's a while old

struct file_hit {
	char path[PATH_MAX];
	bool dir;
	int score;
};

void files_start(void);
// indexed again if it's been a while (in the background)
void files_refresh(void);
// the best max for what's typed (folded), best first: how many
int files_search(const char *query, struct file_hit *out, int max);
void files_stop(void);

#endif
