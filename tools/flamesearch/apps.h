#ifndef FLAMESEARCH_APPS_H
#define FLAMESEARCH_APPS_H

#include <stdbool.h>

// the apps there are to open (their .desktop files, the ones that want to
// be shown), as they change

struct app_hit {
	int index;
	int score;
};

void apps_load(void);
// installed or removed since: looked at again
void apps_refresh(void);
// the best max for what's typed (folded), best first: how many
int apps_search(const char *query, struct app_hit *out, int max);
const char *apps_name(int index);
const char *apps_icon(int index); // an icon name or a file, "" for none
const char *apps_id(int index);
// opened: false when it couldn't be
bool apps_open(int index);

#endif
