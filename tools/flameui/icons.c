#include <gio/gio.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "internal.h"

// icon names to files, the way the icon theme spec has it (the parts that
// matter here): the theme the desktop's set to, the ones it inherits from,
// hicolor last, then pixmaps. a theme's index.theme says its folders and
// the size each holds, the file nearest the size asked for wins (a
// scalable one fits any). found once, remembered

#define THEMES 8
#define BASES 8

enum dir_type { DIR_FIXED, DIR_SCALABLE, DIR_THRESHOLD };

struct icon_dir {
	char *path; // in the theme, "48x48/apps"
	int size, min, max, threshold;
	enum dir_type type;
};

struct icon_theme {
	char name[128];
	struct icon_dir *dirs;
	int len;
};

struct found {
	struct found *next;
	char *name;
	int size;
	char *path; // NULL: there's none
};

struct fui_icons {
	char bases[BASES][PATH_MAX]; // where themes are: ~/.local/share/icons...
	int bases_len;
	struct icon_theme themes[THEMES]; // the order they're looked in
	int themes_len;
	// every other theme there is, read the first time a name's in none of
	// those: an app's icon only in a theme that isn't the one set still
	// beats none
	struct icon_theme others[THEMES * 2];
	int others_len;
	bool others_read;
	struct found *found;
};

static bool is_file(const char *path) {
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void add_base(struct fui_icons *icons, const char *dir, const char *under) {
	if (icons->bases_len < BASES && dir && *dir) {
		snprintf(icons->bases[icons->bases_len++], PATH_MAX, "%s%s", dir, under);
	}
}

static void find_bases(struct fui_icons *icons) {
	const char *home = getenv("HOME"), *data = getenv("XDG_DATA_HOME");
	char path[PATH_MAX];
	if (data && *data) {
		add_base(icons, data, "/icons");
	} else if (home) {
		snprintf(path, sizeof(path), "%s/.local/share", home);
		add_base(icons, path, "/icons");
	}
	if (home) {
		add_base(icons, home, "/.icons");
	}
	const char *dirs = getenv("XDG_DATA_DIRS");
	char list[4096];
	snprintf(list, sizeof(list), "%s", dirs && *dirs ? dirs : "/usr/local/share:/usr/share");
	for (char *save, *d = strtok_r(list, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
		add_base(icons, d, "/icons");
	}
}

// the theme the desktop's set to: gtk's setting (gsettings, else its
// settings.ini). false for none
static bool desktop_theme(char *out, size_t size) {
	GSettingsSchemaSource *source = g_settings_schema_source_get_default();
	GSettingsSchema *schema = source
		? g_settings_schema_source_lookup(source, "org.gnome.desktop.interface", TRUE) : NULL;
	if (schema) {
		g_settings_schema_unref(schema);
		GSettings *settings = g_settings_new("org.gnome.desktop.interface");
		char *theme = g_settings_get_string(settings, "icon-theme");
		snprintf(out, size, "%s", theme ? theme : "");
		g_free(theme);
		g_object_unref(settings);
		if (*out) {
			return true;
		}
	}
	const char *config = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
	char path[PATH_MAX];
	if (config && *config) {
		snprintf(path, sizeof(path), "%s/gtk-3.0/settings.ini", config);
	} else {
		snprintf(path, sizeof(path), "%s/.config/gtk-3.0/settings.ini", home ? home : "");
	}
	FILE *f = fopen(path, "r");
	if (!f) {
		return false;
	}
	char line[512];
	bool got = false;
	while (!got && fgets(line, sizeof(line), f)) {
		char value[256];
		got = sscanf(line, " gtk-icon-theme-name = %255[^\n]", value) == 1;
		if (got) {
			snprintf(out, size, "%s", value);
		}
	}
	fclose(f);
	return got;
}

static char *trim(char *s) {
	while (*s == ' ' || *s == '\t') {
		s++;
	}
	size_t len = strlen(s);
	while (len && (s[len - 1] == '\n' || s[len - 1] == '\r' || s[len - 1] == ' ')) {
		s[--len] = '\0';
	}
	return s;
}

static struct icon_dir *dir_named(struct icon_theme *theme, const char *name) {
	for (int i = 0; i < theme->len; i++) {
		if (!strcmp(theme->dirs[i].path, name)) {
			return &theme->dirs[i];
		}
	}
	return NULL;
}

// name's index.theme, in the first place it's found: its folders into
// theme, what it inherits into inherits. false when there's no such theme
static bool read_theme(struct fui_icons *icons, const char *name, struct icon_theme *theme,
		char *inherits, size_t inherits_size) {
	FILE *f = NULL;
	for (int i = 0; i < icons->bases_len && !f; i++) {
		char path[PATH_MAX];
		snprintf(path, sizeof(path), "%s/%s/index.theme", icons->bases[i], name);
		f = fopen(path, "r");
	}
	if (!f) {
		return false;
	}
	snprintf(theme->name, sizeof(theme->name), "%s", name);
	*inherits = '\0';
	char line[8192], section[256] = "";
	struct icon_dir *dir = NULL;
	while (fgets(line, sizeof(line), f)) {
		char *s = trim(line);
		if (*s == '[') {
			char *end = strchr(s, ']');
			if (end) {
				*end = '\0';
			}
			snprintf(section, sizeof(section), "%s", s + 1);
			dir = strcmp(section, "Icon Theme") ? dir_named(theme, section) : NULL;
			continue;
		}
		char *eq = strchr(s, '=');
		if (!eq) {
			continue;
		}
		*eq = '\0';
		char *key = trim(s), *value = trim(eq + 1);
		if (!strcmp(section, "Icon Theme")) {
			if (!strcmp(key, "Inherits")) {
				snprintf(inherits, inherits_size, "%s", value);
			} else if (!strcmp(key, "Directories")) {
				// one for each, filled in by their sections
				int n = 1;
				for (const char *c = value; *c; c++) {
					n += *c == ',';
				}
				theme->dirs = calloc((size_t)n, sizeof(*theme->dirs));
				for (char *save, *d = strtok_r(value, ",", &save); d && theme->dirs;
						d = strtok_r(NULL, ",", &save)) {
					struct icon_dir *add = &theme->dirs[theme->len];
					if ((add->path = strdup(trim(d)))) {
						add->threshold = 2;
						theme->len++;
					}
				}
			}
		} else if (dir) {
			int n = atoi(value);
			if (!strcmp(key, "Size")) {
				dir->size = n;
			} else if (!strcmp(key, "MinSize")) {
				dir->min = n;
			} else if (!strcmp(key, "MaxSize")) {
				dir->max = n;
			} else if (!strcmp(key, "Threshold")) {
				dir->threshold = n;
			} else if (!strcmp(key, "Scale") && n > 1) {
				dir->size = -1; // for screens twice as dense: not looked in
			} else if (!strcmp(key, "Type")) {
				dir->type = !strcmp(value, "Scalable") ? DIR_SCALABLE
					: !strcmp(value, "Threshold") ? DIR_THRESHOLD : DIR_FIXED;
			}
		}
	}
	fclose(f);
	for (int i = 0; i < theme->len; i++) {
		struct icon_dir *d = &theme->dirs[i];
		d->min = d->min ? d->min : d->size;
		d->max = d->max ? d->max : d->size;
	}
	return true;
}

// the theme and the ones it inherits from (each once), in order
static void add_theme(struct fui_icons *icons, const char *name, int depth) {
	if (depth > 4 || icons->themes_len == THEMES || !strcmp(name, "hicolor")) {
		return; // hicolor's last, whoever inherits it
	}
	for (int i = 0; i < icons->themes_len; i++) {
		if (!strcmp(icons->themes[i].name, name)) {
			return;
		}
	}
	char inherits[512];
	struct icon_theme *theme = &icons->themes[icons->themes_len];
	if (!read_theme(icons, name, theme, inherits, sizeof(inherits))) {
		return;
	}
	icons->themes_len++;
	for (char *save, *parent = strtok_r(inherits, ",", &save); parent;
			parent = strtok_r(NULL, ",", &save)) {
		add_theme(icons, trim(parent), depth + 1);
	}
}

static struct fui_icons *icons_of(struct fui *ui) {
	if (ui->icons) {
		return ui->icons;
	}
	struct fui_icons *icons = calloc(1, sizeof(*icons));
	if (!icons) {
		return NULL;
	}
	find_bases(icons);
	char name[128];
	if (desktop_theme(name, sizeof(name))) {
		add_theme(icons, name, 0);
	}
	if (icons->themes_len < THEMES) {
		char inherits[8];
		if (read_theme(icons, "hicolor", &icons->themes[icons->themes_len], inherits,
				sizeof(inherits))) {
			icons->themes_len++;
		}
	}
	ui->icons = icons;
	return icons;
}

// how far a folder's icons are from size, 0 for right
static int distance(const struct icon_dir *dir, int size) {
	switch (dir->type) {
	case DIR_FIXED:
		return abs(dir->size - size);
	case DIR_SCALABLE:
		return size < dir->min ? dir->min - size : size > dir->max ? size - dir->max : 0;
	case DIR_THRESHOLD:
		return size < dir->size - dir->threshold ? dir->size - dir->threshold - size
			: size > dir->size + dir->threshold ? size - dir->size - dir->threshold : 0;
	}
	return 0;
}

// name's file in a theme's folder, from any of the places themes are
static bool in_dir(struct fui_icons *icons, const struct icon_theme *theme,
		const struct icon_dir *dir, const char *name, char *out, size_t size) {
	static const char *const kinds[] = {"png", "svg"};
	for (int i = 0; i < icons->bases_len; i++) {
		for (int k = 0; k < 2; k++) {
			snprintf(out, size, "%s/%s/%s/%s.%s", icons->bases[i], theme->name, dir->path, name,
				kinds[k]);
			if (is_file(out)) {
				return true;
			}
		}
	}
	return false;
}

static bool known(const struct fui_icons *icons, const char *name) {
	for (int i = 0; i < icons->themes_len; i++) {
		if (!strcmp(icons->themes[i].name, name)) {
			return true;
		}
	}
	for (int i = 0; i < icons->others_len; i++) {
		if (!strcmp(icons->others[i].name, name)) {
			return true;
		}
	}
	return false;
}

static void read_others(struct fui_icons *icons) {
	icons->others_read = true;
	for (int i = 0; i < icons->bases_len; i++) {
		DIR *dir = opendir(icons->bases[i]);
		struct dirent *e;
		while (dir && (e = readdir(dir)) && icons->others_len < THEMES * 2) {
			char inherits[8];
			struct icon_theme *theme = &icons->others[icons->others_len];
			if (e->d_name[0] != '.' && !known(icons, e->d_name)
					&& read_theme(icons, e->d_name, theme, inherits, sizeof(inherits))) {
				if (theme->len) {
					icons->others_len++;
				} else {
					free(theme->dirs); // a cursor theme: no icons in it
					*theme = (struct icon_theme){0};
				}
			}
		}
		if (dir) {
			closedir(dir);
		}
	}
}

// name in the best of theme's folders for size, into out
static bool in_theme(struct fui_icons *icons, const struct icon_theme *theme, const char *name,
		int size, char *out, size_t out_size) {
	char path[PATH_MAX];
	int best = INT_MAX;
	for (int i = 0; i < theme->len && best; i++) {
		const struct icon_dir *dir = &theme->dirs[i];
		// one smaller blown up is blurred: a bigger one's shrunk instead
		int d = dir->size < 0 ? INT_MAX : distance(dir, size);
		if (d && d != INT_MAX && dir->type != DIR_SCALABLE && dir->size < size) {
			d += 4096;
		}
		if (d < best && in_dir(icons, theme, dir, name, path, sizeof(path))) {
			best = d;
			snprintf(out, out_size, "%s", path);
		}
	}
	return best != INT_MAX;
}

static bool look_up(struct fui_icons *icons, const char *name, int size, char *out,
		size_t out_size) {
	for (int t = 0; t < icons->themes_len; t++) {
		if (in_theme(icons, &icons->themes[t], name, size, out, out_size)) {
			return true;
		}
	}
	static const char *const kinds[] = {"png", "svg"};
	for (int k = 0; k < 2; k++) {
		snprintf(out, out_size, "/usr/share/pixmaps/%s.%s", name, kinds[k]);
		if (is_file(out)) {
			return true;
		}
	}
	if (!icons->others_read) {
		read_others(icons);
	}
	for (int t = 0; t < icons->others_len; t++) {
		if (in_theme(icons, &icons->others[t], name, size, out, out_size)) {
			return true;
		}
	}
	return false;
}

// name's file for size: remembered, NULL for none
static const char *icon_path(struct fui *ui, const char *name, int size) {
	struct fui_icons *icons = icons_of(ui);
	if (!icons) {
		return NULL;
	}
	for (struct found *f = icons->found; f; f = f->next) {
		if (f->size == size && !strcmp(f->name, name)) {
			return f->path;
		}
	}
	struct found *f = calloc(1, sizeof(*f));
	if (!f || !(f->name = strdup(name))) {
		free(f);
		return NULL;
	}
	char path[PATH_MAX];
	f->size = size;
	f->path = !strchr(name, '/') && look_up(icons, name, size, path, sizeof(path))
		? strdup(path) : NULL;
	f->next = icons->found;
	icons->found = f;
	return f->path;
}

cairo_surface_t *fui_icon(struct fui *ui, const char *name, int size, bool *failed) {
	bool ignored;
	failed = failed ? failed : &ignored;
	*failed = true;
	if (!name || !*name) {
		return NULL;
	}
	// a file's path rather than a name
	if (!strncmp(name, "file://", 7)) {
		name += 7;
	}
	const char *path = name[0] == '/' ? name : icon_path(ui, name, size);
	if (!path) {
		return NULL;
	}
	return fui_thumb_fit(ui, path, size, failed);
}

bool fui_app_icon(const char *app_id, char *name, size_t size) {
	if (!app_id || !*app_id || strchr(app_id, '/')) {
		return false;
	}
	// the user's own first, then the system's
	const char *home = getenv("HOME"), *data = getenv("XDG_DATA_HOME");
	const char *dirs = getenv("XDG_DATA_DIRS");
	char own[PATH_MAX], list[4096];
	if (data && *data) {
		snprintf(own, sizeof(own), "%s", data);
	} else {
		snprintf(own, sizeof(own), "%s/.local/share", home ? home : "");
	}
	snprintf(list, sizeof(list), "%s:%s", own, dirs && *dirs ? dirs
		: "/usr/local/share:/usr/share");
	for (char *save, *d = strtok_r(list, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
		char path[PATH_MAX];
		snprintf(path, sizeof(path), "%s/applications/%s%s", d, app_id,
			strstr(app_id, ".desktop") ? "" : ".desktop");
		FILE *f = fopen(path, "r");
		if (!f) {
			continue;
		}
		char line[1024];
		bool entry = false, got = false;
		while (!got && fgets(line, sizeof(line), f)) {
			char *s = trim(line);
			if (*s == '[') {
				entry = !strcmp(s, "[Desktop Entry]");
			} else if (entry && !strncmp(s, "Icon=", 5)) {
				snprintf(name, size, "%s", trim(s + 5));
				got = *name;
			}
		}
		fclose(f);
		if (got) {
			return true;
		}
	}
	return false;
}

void fui_icons_free(struct fui *ui) {
	struct fui_icons *icons = ui->icons;
	if (!icons) {
		return;
	}
	for (int t = 0; t < icons->themes_len; t++) {
		for (int i = 0; i < icons->themes[t].len; i++) {
			free(icons->themes[t].dirs[i].path);
		}
		free(icons->themes[t].dirs);
	}
	for (int t = 0; t < icons->others_len; t++) {
		for (int i = 0; i < icons->others[t].len; i++) {
			free(icons->others[t].dirs[i].path);
		}
		free(icons->others[t].dirs);
	}
	while (icons->found) {
		struct found *f = icons->found;
		icons->found = f->next;
		free(f->name);
		free(f->path);
		free(f);
	}
	free(icons);
	ui->icons = NULL;
}
