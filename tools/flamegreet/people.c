#include "people.h"

#include <dirent.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// login.defs' usual range: below it the system's own accounts, above it nobody
#define UID_MIN 1000
#define UID_MAX 60000

static bool can_log_in(const struct passwd *pw) {
	if (pw->pw_uid < UID_MIN || pw->pw_uid > UID_MAX || !pw->pw_shell) {
		return false;
	}
	const char *shell = strrchr(pw->pw_shell, '/');
	shell = shell ? shell + 1 : pw->pw_shell;
	return *shell && strcmp(shell, "nologin") && strcmp(shell, "false");
}

static int by_name(const void *a, const void *b) {
	return strcmp(((const struct session *)a)->name, ((const struct session *)b)->name);
}

// the [Desktop Entry] keys a session needs. false: it's not one to show
static bool read_session(const char *path, struct session *session) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}
	char line[1024];
	bool entry = false, hidden = false;
	session->name[0] = session->exec[0] = session->desktops[0] = '\0';
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (line[0] == '[') {
			entry = !strcmp(line, "[Desktop Entry]");
			continue;
		}
		if (!entry) {
			continue;
		}
		char *eq = strchr(line, '=');
		if (!eq) {
			continue;
		}
		*eq = '\0';
		const char *value = eq + 1;
		if (!strcmp(line, "Name")) {
			snprintf(session->name, sizeof(session->name), "%s", value);
		} else if (!strcmp(line, "Exec")) {
			snprintf(session->exec, sizeof(session->exec), "%s", value);
		} else if (!strcmp(line, "DesktopNames")) {
			snprintf(session->desktops, sizeof(session->desktops), "%s", value);
			for (char *c = session->desktops; *c; c++) {
				*c = *c == ';' ? ':' : *c;
			}
			size_t len = strlen(session->desktops);
			if (len && session->desktops[len - 1] == ':') {
				session->desktops[len - 1] = '\0';
			}
		} else if ((!strcmp(line, "Hidden") || !strcmp(line, "NoDisplay"))
				&& !strcmp(value, "true")) {
			hidden = true;
		}
	}
	fclose(f);
	// field codes (%f, %U...) mean nothing for a session
	for (char *c = strchr(session->exec, '%'); c; c = strchr(c, '%')) {
		memmove(c, c + 2 - !c[1], strlen(c + 2 - !c[1]) + 1);
	}
	return !hidden && *session->exec && *session->name;
}

static void load_sessions(struct people *people) {
	// $XDG_DATA_DIRS' wayland-sessions, the first one with a name wins.
	// FLAMEGREET_SESSIONS: the folders themselves instead (the tests')
	const char *dirs = getenv("FLAMEGREET_SESSIONS");
	const char *data = getenv("XDG_DATA_DIRS");
	char list[4096];
	snprintf(list, sizeof(list), "%s", dirs ? dirs
		: data && *data ? data : "/usr/local/share:/usr/share");
	char *save = NULL;
	for (char *dir = strtok_r(list, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
		char path[4096];
		snprintf(path, sizeof(path), "%s%s", dir, dirs ? "" : "/wayland-sessions");
		DIR *d = opendir(path);
		if (!d) {
			continue;
		}
		struct dirent *e;
		while ((e = readdir(d)) && people->sessions < SESSIONS_MAX) {
			size_t len = strlen(e->d_name);
			if (len < 9 || strcmp(e->d_name + len - 8, ".desktop")) {
				continue;
			}
			struct session *s = &people->session[people->sessions];
			snprintf(s->id, sizeof(s->id), "%.*s", (int)(len - 8), e->d_name);
			bool seen = false;
			for (int i = 0; i < people->sessions; i++) {
				seen |= !strcmp(people->session[i].id, s->id);
			}
			char file[8192];
			snprintf(file, sizeof(file), "%s/%s", path, e->d_name);
			if (!seen && read_session(file, s)) {
				people->sessions++;
			}
		}
		closedir(d);
	}
	qsort(people->session, (size_t)people->sessions, sizeof(people->session[0]), by_name);
	// none installed: flame itself
	if (!people->sessions) {
		struct session *s = &people->session[people->sessions++];
		snprintf(s->id, sizeof(s->id), "flamewm");
		snprintf(s->name, sizeof(s->name), "flame");
		snprintf(s->exec, sizeof(s->exec), "flamewm");
		snprintf(s->desktops, sizeof(s->desktops), "flamewm");
	}
}

// where the last picks are kept: the greeter's own home, when it has one
static bool last_path(char *path, size_t size) {
	const char *cache = getenv("XDG_CACHE_HOME");
	const char *home = getenv("HOME");
	if (cache && *cache) {
		snprintf(path, size, "%s/flamegreet", cache);
	} else if (home && *home) {
		snprintf(path, size, "%s/.cache/flamegreet", home);
	} else {
		return false;
	}
	return true;
}

static void load_last(struct people *people) {
	char path[4096];
	if (!last_path(path, sizeof(path))) {
		return;
	}
	strncat(path, "/last", sizeof(path) - strlen(path) - 1);
	FILE *f = fopen(path, "r");
	if (!f) {
		return;
	}
	char user[128] = "", session[128] = "";
	if (fscanf(f, "%127s %127s", user, session) >= 1) {
		for (int i = 0; i < people->len; i++) {
			if (!strcmp(people->person[i].user, user)) {
				people->last_person = i;
			}
		}
		for (int i = 0; i < people->sessions; i++) {
			if (!strcmp(people->session[i].id, session)) {
				people->last_session = i;
			}
		}
	}
	fclose(f);
}

void people_load(struct people *people) {
	people->len = people->sessions = 0;
	people->last_person = people->last_session = 0;
	setpwent();
	struct passwd *pw;
	while ((pw = getpwent()) && people->len < PEOPLE_MAX) {
		if (can_log_in(pw)) {
			look_person(&people->person[people->len++], pw);
		}
	}
	endpwent();
	load_sessions(people);
	// flame first, when nothing was picked before
	for (int i = 0; i < people->sessions; i++) {
		if (strstr(people->session[i].id, "flame")) {
			people->last_session = i;
			break;
		}
	}
	load_last(people);
}

void people_picked(const struct people *people, int person, int session) {
	char path[4096];
	if (!last_path(path, sizeof(path))) {
		return;
	}
	// the folder and the one it's in (~/.cache), a greeter's home starts bare
	char *slash = strrchr(path, '/');
	if (slash) {
		*slash = '\0';
		mkdir(path, 0700);
		*slash = '/';
	}
	mkdir(path, 0700);
	strncat(path, "/last", sizeof(path) - strlen(path) - 1);
	FILE *f = fopen(path, "w");
	if (!f) {
		return;
	}
	fprintf(f, "%s %s\n", people->person[person].user, people->session[session].id);
	fclose(f);
}
