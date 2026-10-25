#include "config/starter.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wlr/util/log.h>

#include "config/config.h"
#include "util/spawn.h"

// what a first flame.lua says: the few things people change first, in
// comments, and the bar when flame's is installed
static const char starter_top[] =
	"-- flame's config, lua. save it and flame picks it up right away.\n"
	"-- every option: https://github.com/haiderking1/flamewm/blob/main/examples/flame.lua\n"
	"\n";

static const char starter_bar[] =
	"-- the bar (and with it notifications, the media keys, the battery)\n"
	"flame.autostart(\"flamebar\")\n"
	"\n";

static const char starter_no_bar[] =
	"-- a bar of your own\n"
	"-- flame.autostart(\"waybar\")\n"
	"\n";

static const char starter_rest[] =
	"-- how it looks\n"
	"-- flame.look { gap = 8, border_width = 2 }\n"
	"\n"
	"-- the keyboard\n"
	"-- flame.keyboard { layout = \"us\" }\n"
	"\n"
	"-- what super+l runs: \"swaylock\", \"\" for nothing\n"
	"-- flame.lockscreen { command = \"flamelock\" }\n"
	"\n"
	"-- your own binds: keys, and what they run\n"
	"-- flame.bind(\"super\", \"b\", \"firefox\")\n";

// path's folder, made if it isn't there (and the ones above it)
static bool make_folder(const char *path) {
	char folder[512];
	snprintf(folder, sizeof(folder), "%s", path);
	char *slash = strrchr(folder, '/');
	if (!slash || slash == folder) {
		return true;
	}
	*slash = '\0';
	for (char *p = folder + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(folder, 0755);
			*p = '/';
		}
	}
	return mkdir(folder, 0755) == 0 || errno == EEXIST;
}

void config_first_start(void) {
	const char *path = config_default_path();
	if (access(path, F_OK) == 0) {
		return;
	}
	// it used to be init.lua: moved over once, nothing in it lost
	char old[512];
	snprintf(old, sizeof(old), "%s", path);
	char *slash = strrchr(old, '/');
	if (slash && (size_t)(slash + 1 - old) + sizeof("init.lua") <= sizeof(old)) {
		strcpy(slash + 1, "init.lua");
		if (access(old, F_OK) == 0) {
			if (rename(old, path) == 0) {
				wlr_log(WLR_INFO, "config: %s is %s now", old, path);
			} else {
				wlr_log_errno(WLR_ERROR, "config: couldn't move %s to %s", old, path);
			}
			return;
		}
	}
	if (!make_folder(path)) {
		wlr_log_errno(WLR_ERROR, "config: no folder for %s", path);
		return;
	}
	FILE *f = fopen(path, "wx");
	if (!f) {
		wlr_log_errno(WLR_ERROR, "config: couldn't write a first %s", path);
		return;
	}
	fputs(starter_top, f);
	fputs(spawn_findable("flamebar") ? starter_bar : starter_no_bar, f);
	fputs(starter_rest, f);
	if (fclose(f) == 0) {
		wlr_log(WLR_INFO, "config: wrote a first %s", path);
	}
}
