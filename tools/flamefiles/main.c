// flamefiles: flame's file manager, on flame's glass. a sidebar of places,
// a folder as icons or a list, the usual things done to files. with --pick
// it's a chooser: what the user picks is printed, for the app that asked
// (flamesettings' wallpaper, later every app's open dialog through flame's
// portal)

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "../flameui/flameui.h"
#include "folder.h"
#include "icons.h"
#include "ops.h"

// the window's size, what flame made it: read every frame
static int W = 1000, H = 640;
#define MIN_W 640
#define MIN_H 400
#define SIDEBAR_W 200
#define TOOLBAR_H 52
#define STATUS_H 30
#define VIEW_X (SIDEBAR_W + 1)
#define VIEW_W (W - VIEW_X)
#define VIEW_TOP TOOLBAR_H
#define VIEW_BOTTOM (H - STATUS_H)

#define TEXT 0xffffffeb
#define TEXT_DIM 0xffffff8c
#define TEXT_FAINT 0xffffff59
#define ACCENT 0xff7a2aff
#define CONTENT_BG 0x1c1c20ff

#define F_TITLE "Inter Semi-Bold 12.5"
#define F_TEXT "Inter 10"
#define F_MEDIUM "Inter Medium 10"
#define F_SMALL "Inter 9"
#define F_HEAD "Inter Semi-Bold 8.5"

// the zoom: icons (and their cells round them) and list rows, as big as
// they are right now, easing toward what's asked
#define ICON (app->icon)
#define CELL_W (app->icon + 48)
#define CELL_H (app->icon + 54)
#define ROW_H (app->row)
#define ICON_DEFAULT 64
#define ROW_DEFAULT 24
#define LIST_HEAD 28
#define GRID_PAD 14

struct place_item {
	enum place icon;
	char name[64];
	char path[PATH_MAX];
	bool location; // under Locations, not Favorites
};

struct app {
	struct fui *ui;
	struct folder folder;
	// what's shown: indexes into folder.files, hidden ones and what the
	// search doesn't match left out
	size_t *shown;
	size_t shown_len;
	bool *selected; // by folder.files index
	size_t anchor, cursor; // in shown
	bool has_cursor;
	char **back, **forward;
	size_t back_len, forward_len;
	bool list_view, hidden;
	enum sort_key sort;
	bool descending;
	char query[64], searched[64]; // searched: what's shown is for it
	double scroll;
	int cols; // icon columns last frame, for the arrows
	double icon, icon_want, row, row_want;

	// a name being typed over
	bool renaming;
	char rename_from[NAME_MAX + 1], rename_text[NAME_MAX + 1];
	// a paste asked for (the clipboard's read on a later frame): where to
	char paste_into[PATH_MAX];
	struct ops_job *job;
	char toast[220];
	double toast_at;
	// dragging a box over files
	bool banding;
	double band_x, band_y; // where it started, folder coords (scroll added)
	// typing a name's first letters picks it
	char typeahead[64];
	double typeahead_at;
	// selected once the folder's listed again (a new folder, what was pasted)
	char select_next[NAME_MAX + 1];
	bool rename_next;
	// asking before deleting for good
	bool confirming;

	// a press on a file: a drag once it moves far enough, else on release
	// just that one picked (a click on one of several selected)
	long press_item; // shown index + 1, 0 for none
	double press_x, press_y;
	bool press_narrow;
	// what a drag over the window drops into: a folder shown (index + 1),
	// a place, the folder open
	long drop_item;
	int drop_place;
	bool drop_here;
	double view_top; // last frame's: where the files were drawn from
	double place_y[32];

	// --pick: choosing a file (pictures only), or a folder
	bool pick, pick_images, pick_folder;
	int status;

	struct place_item places[32];
	int places_len;
	char trash[PATH_MAX];
};

static const struct file *file_at(struct app *app, size_t shown_index) {
	return &app->folder.files[app->shown[shown_index]];
}

static void path_of(struct app *app, const char *name, char *out, size_t size) {
	snprintf(out, size, "%s%s%s", app->folder.path, strcmp(app->folder.path, "/") ? "/" : "",
		name);
}

static void toast(struct app *app, const char *fmt, const char *what) {
	snprintf(app->toast, sizeof(app->toast), fmt, what);
	app->toast_at = fui_time(app->ui);
	fui_redraw(app->ui);
}

static bool in_trash(struct app *app) {
	return !strcmp(app->folder.path, app->trash);
}

// ---- places ----

static const char *home_dir(void) {
	const char *home = getenv("HOME");
	return home && *home ? home : "/";
}

static void add_place(struct app *app, enum place icon, const char *name, const char *path,
		bool location) {
	struct stat st;
	if (app->places_len >= (int)(sizeof(app->places) / sizeof(app->places[0]))
			|| stat(path, &st) < 0 || !S_ISDIR(st.st_mode)) {
		return;
	}
	for (int i = 0; i < app->places_len; i++) {
		if (!strcmp(app->places[i].path, path)) {
			return;
		}
	}
	struct place_item *p = &app->places[app->places_len++];
	p->icon = icon;
	p->location = location;
	snprintf(p->name, sizeof(p->name), "%s", name);
	snprintf(p->path, sizeof(p->path), "%s", path);
}

// the user's folders (xdg-user-dirs), their bookmarks (gtk's), the drive
static void places_load(struct app *app) {
	const char *home = home_dir();
	add_place(app, PLACE_HOME, "Home", home, false);
	static const struct {
		const char *key, *fallback, *name;
		enum place icon;
	} dirs[] = {
		{"DESKTOP", "Desktop", "Desktop", PLACE_DESKTOP},
		{"DOCUMENTS", "Documents", "Documents", PLACE_DOCUMENTS},
		{"DOWNLOAD", "Downloads", "Downloads", PLACE_DOWNLOADS},
		{"PICTURES", "Pictures", "Pictures", PLACE_PICTURES},
		{"MUSIC", "Music", "Music", PLACE_MUSIC},
		{"VIDEOS", "Videos", "Videos", PLACE_VIDEOS},
	};
	char user_dirs[PATH_MAX];
	const char *config = getenv("XDG_CONFIG_HOME");
	snprintf(user_dirs, sizeof(user_dirs), "%s/user-dirs.dirs", config && *config == '/'
		? config : "");
	if (!config || *config != '/') {
		snprintf(user_dirs, sizeof(user_dirs), "%s/.config/user-dirs.dirs", home);
	}
	for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		char path[PATH_MAX];
		snprintf(path, sizeof(path), "%s/%s", home, dirs[i].fallback);
		FILE *f = fopen(user_dirs, "re");
		char line[PATH_MAX + 64], want[64];
		snprintf(want, sizeof(want), "XDG_%s_DIR=\"", dirs[i].key);
		while (f && fgets(line, sizeof(line), f)) {
			if (strncmp(line, want, strlen(want))) {
				continue;
			}
			char *v = line + strlen(want);
			v[strcspn(v, "\"")] = '\0';
			if (!strncmp(v, "$HOME", 5)) {
				snprintf(path, sizeof(path), "%s%s", home, v + 5);
			} else if (*v == '/') {
				snprintf(path, sizeof(path), "%s", v);
			}
		}
		if (f) {
			fclose(f);
		}
		if (strcmp(path, home)) {
			add_place(app, dirs[i].icon, dirs[i].name, path, false);
		}
	}
	// bookmarks: file:///path name
	char bookmarks[PATH_MAX];
	snprintf(bookmarks, sizeof(bookmarks), "%s/.config/gtk-3.0/bookmarks", home);
	FILE *f = fopen(bookmarks, "re");
	char line[PATH_MAX + 128];
	while (f && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = '\0';
		if (strncmp(line, "file://", 7)) {
			continue;
		}
		char *uri = line + 7, *label = strchr(uri, ' ');
		if (label) {
			*label++ = '\0';
		}
		char path[PATH_MAX];
		size_t n = 0;
		for (const char *c = uri; *c && n + 1 < sizeof(path); c++) {
			unsigned v;
			if (*c == '%' && sscanf(c + 1, "%2x", &v) == 1) {
				path[n++] = (char)v;
				c += 2;
			} else {
				path[n++] = *c;
			}
		}
		path[n] = '\0';
		const char *slash = strrchr(path, '/');
		add_place(app, PLACE_FOLDER, label && *label ? label : slash ? slash + 1 : path, path,
			false);
	}
	if (f) {
		fclose(f);
	}
	add_place(app, PLACE_COMPUTER, "Computer", "/", true);
	// there to be shown, even before anything's thrown away
	ops_trash_dir(app->trash, sizeof(app->trash));
	add_place(app, PLACE_TRASH, "Trash", app->trash, true);
}

static enum place place_of(struct app *app, const char *path) {
	for (int i = 0; i < app->places_len; i++) {
		if (!strcmp(app->places[i].path, path)) {
			return app->places[i].icon;
		}
	}
	return PLACE_NONE;
}

// ---- what's shown ----

static bool matches(const char *name, const char *query) {
	for (const char *q = query; *q;) {
		while (*q == ' ') {
			q++;
		}
		size_t len = strcspn(q, " ");
		bool found = false;
		for (const char *h = name; len && *h && !found; h++) {
			found = !strncasecmp(h, q, len);
		}
		if (len && !found) {
			return false;
		}
		q += len;
	}
	return true;
}

// sorted again, and what's shown worked out: selection kept by name
static void rebuild(struct app *app) {
	struct folder *folder = &app->folder;
	folder_sort(folder, app->sort, app->descending);
	free(app->shown);
	app->shown = calloc(folder->len ? folder->len : 1, sizeof(*app->shown));
	app->shown_len = 0;
	for (size_t i = 0; app->shown && i < folder->len; i++) {
		const struct file *f = &folder->files[i];
		if ((f->hidden && !app->hidden) || !matches(f->name, app->query)) {
			continue;
		}
		app->shown[app->shown_len++] = i;
	}
	if (app->cursor >= app->shown_len) {
		app->has_cursor = false;
	}
}

static void selection_clear(struct app *app) {
	if (app->selected) {
		memset(app->selected, 0, app->folder.len * sizeof(*app->selected));
	}
}

static size_t selected_count(struct app *app) {
	size_t n = 0;
	for (size_t i = 0; i < app->shown_len; i++) {
		n += app->selected[app->shown[i]];
	}
	return n;
}

// listed again: what was selected still is, by name
static void relist(struct app *app) {
	struct folder *folder = &app->folder;
	char **keep = NULL;
	size_t keep_len = 0;
	for (size_t i = 0; app->selected && i < folder->len; i++) {
		if (app->selected[i]) {
			char **grown = realloc(keep, (keep_len + 1) * sizeof(*keep));
			if (grown) {
				keep = grown;
				keep[keep_len++] = strdup(folder->files[i].name);
			}
		}
	}
	char cursor_name[NAME_MAX + 1] = "";
	if (app->has_cursor && app->cursor < app->shown_len) {
		snprintf(cursor_name, sizeof(cursor_name), "%s", file_at(app, app->cursor)->name);
	}
	if (!folder_list(folder, folder->path)) {
		// gone: up to the nearest folder that's still there
		char up[PATH_MAX];
		snprintf(up, sizeof(up), "%s", folder->path);
		char *slash;
		while ((slash = strrchr(up, '/')) && slash != up) {
			*slash = '\0';
			if (folder_list(folder, up)) {
				break;
			}
		}
	}
	free(app->selected);
	app->selected = calloc(folder->len ? folder->len : 1, sizeof(*app->selected));
	for (size_t i = 0; i < folder->len && app->selected; i++) {
		for (size_t k = 0; k < keep_len; k++) {
			if (keep[k] && !strcmp(keep[k], folder->files[i].name)) {
				app->selected[i] = true;
			}
		}
	}
	for (size_t k = 0; k < keep_len; k++) {
		free(keep[k]);
	}
	free(keep);
	rebuild(app);
	const char *want = app->select_next[0] ? app->select_next : cursor_name;
	for (size_t i = 0; *want && i < app->shown_len; i++) {
		if (!strcmp(file_at(app, i)->name, want)) {
			if (app->select_next[0]) {
				selection_clear(app);
				app->selected[app->shown[i]] = true;
				app->anchor = i;
			}
			app->cursor = i;
			app->has_cursor = true;
		}
	}
	if (app->select_next[0] && app->rename_next && app->has_cursor) {
		app->renaming = true;
		snprintf(app->rename_from, sizeof(app->rename_from), "%s", app->select_next);
		snprintf(app->rename_text, sizeof(app->rename_text), "%s", app->select_next);
		fui_focus(app->ui, fui_id("rename", 0), app->rename_text);
	}
	app->select_next[0] = '\0';
	app->rename_next = false;
}

// path opened in the window. record: back comes here
static bool go(struct app *app, const char *path, bool record) {
	char before[PATH_MAX];
	snprintf(before, sizeof(before), "%s", app->folder.path);
	if (!folder_list(&app->folder, path)) {
		toast(app, "Can't open %s", path);
		return false;
	}
	if (record && before[0] && strcmp(before, app->folder.path)) {
		char **grown = realloc(app->back, (app->back_len + 1) * sizeof(*app->back));
		if (grown) {
			app->back = grown;
			app->back[app->back_len++] = strdup(before);
		}
		for (size_t i = 0; i < app->forward_len; i++) {
			free(app->forward[i]);
		}
		app->forward_len = 0;
	}
	free(app->selected);
	app->selected = calloc(app->folder.len ? app->folder.len : 1, sizeof(*app->selected));
	app->query[0] = '\0';
	app->scroll = 0;
	app->has_cursor = false;
	app->renaming = false;
	app->banding = false;
	fui_thumbs_forget(app->ui);
	rebuild(app);
	// coming back up: the folder it came from is the one selected
	size_t bl = strlen(app->folder.path);
	if (!strncmp(before, app->folder.path, bl) && before[bl == 1 ? 1 : bl] != '\0'
			&& (bl == 1 || before[bl] == '/')) {
		const char *child = before + (bl == 1 ? 1 : bl + 1);
		size_t len = strcspn(child, "/");
		for (size_t i = 0; i < app->shown_len; i++) {
			const char *name = file_at(app, i)->name;
			if (strlen(name) == len && !strncmp(name, child, len)) {
				app->selected[app->shown[i]] = true;
				app->cursor = app->anchor = i;
				app->has_cursor = true;
			}
		}
	}
	return true;
}

static void history(struct app *app, bool forward) {
	char ***from = forward ? &app->forward : &app->back;
	size_t *from_len = forward ? &app->forward_len : &app->back_len;
	char ***to = forward ? &app->back : &app->forward;
	size_t *to_len = forward ? &app->back_len : &app->forward_len;
	if (!*from_len) {
		return;
	}
	char *path = (*from)[--*from_len];
	char *here = strdup(app->folder.path);
	if (go(app, path, false) && here) {
		char **grown = realloc(*to, (*to_len + 1) * sizeof(**to));
		if (grown) {
			*to = grown;
			(*to)[(*to_len)++] = here;
			here = NULL;
		}
	}
	free(here);
	free(path);
}

static void go_up(struct app *app) {
	char up[PATH_MAX];
	snprintf(up, sizeof(up), "%s", app->folder.path);
	char *slash = strrchr(up, '/');
	if (!slash || !strcmp(up, "/")) {
		return;
	}
	if (slash == up) {
		up[1] = '\0';
	} else {
		*slash = '\0';
	}
	go(app, up, true);
}

// ---- doing things ----

// what --pick takes
static bool pickable(struct app *app, const struct file *f) {
	if (app->pick_folder) {
		return f->kind == KIND_FOLDER;
	}
	return f->kind != KIND_FOLDER && (!app->pick_images || f->kind == KIND_IMAGE);
}

static void picked(struct app *app, const char *path) {
	printf("%s\n", path);
	fflush(stdout);
	app->status = 0;
	fui_quit(app->ui);
}

static void open_file(struct app *app, size_t shown_index) {
	const struct file *f = file_at(app, shown_index);
	char path[PATH_MAX];
	path_of(app, f->name, path, sizeof(path));
	if (f->kind == KIND_FOLDER) {
		go(app, path, true);
	} else if (app->pick) {
		if (pickable(app, f)) {
			picked(app, path);
		}
	} else {
		ops_open(path);
	}
}

// the selected files' paths, in what's shown's order
static char **selected_paths(struct app *app, size_t *len) {
	size_t n = selected_count(app), k = 0;
	char **paths = calloc(n ? n : 1, sizeof(*paths));
	for (size_t i = 0; paths && i < app->shown_len; i++) {
		if (app->selected[app->shown[i]]) {
			char path[PATH_MAX];
			path_of(app, file_at(app, i)->name, path, sizeof(path));
			paths[k++] = strdup(path);
		}
	}
	*len = k;
	return paths;
}

static void free_paths(char **paths, size_t len) {
	for (size_t i = 0; i < len; i++) {
		free(paths[i]);
	}
	free(paths);
}

// the selection on the clipboard, everyone's: other file managers paste it,
// terminals and editors get the paths
static void clip_set(struct app *app, bool cut) {
	size_t len;
	char **paths = selected_paths(app, &len);
	fui_copy_files(app->ui, paths, len, cut);
	free_paths(paths, len);
}

// the selection's paths as text, a line each
static void copy_paths(struct app *app) {
	size_t len, total = 1;
	char **paths = selected_paths(app, &len);
	for (size_t i = 0; i < len; i++) {
		total += strlen(paths[i]) + 1;
	}
	char *text = calloc(total, 1);
	for (size_t i = 0; text && i < len; i++) {
		strcat(text, paths[i]);
		strcat(text, i + 1 < len ? "\n" : "");
	}
	if (text && len) {
		fui_copy_text(app->ui, text);
	}
	free(text);
	free_paths(paths, len);
}

// cut and still on the clipboard: shown faded
static bool clipped(struct app *app, const char *name) {
	char path[PATH_MAX];
	path_of(app, name, path, sizeof(path));
	return fui_clipboard_cut(app->ui, path);
}

static void start_job_into(struct app *app, char *const *paths, size_t len, const char *dest,
		bool move) {
	if (app->job) {
		toast(app, "%s", "Still busy with the last one");
		return;
	}
	app->job = ops_job_start(app->ui, paths, len, dest, move);
	if (!app->job) {
		toast(app, "%s", "Can't start copying");
	}
}

static void start_job(struct app *app, char *const *paths, size_t len, bool move) {
	start_job_into(app, paths, len, app->folder.path, move);
}

// the clipboard's files asked for, into the folder open now
static void paste(struct app *app) {
	if (in_trash(app) || app->pick) {
		return;
	}
	if (fui_paste_files(app->ui)) {
		snprintf(app->paste_into, sizeof(app->paste_into), "%s", app->folder.path);
	}
}

// the files a paste asked for came: copied there, cut ones moved (and the
// clipboard's spent then, like other file managers leave it)
static void pasted(struct app *app) {
	size_t len;
	bool cut;
	char **paths = fui_pasted_files(app->ui, &len, &cut);
	if (!paths || !app->paste_into[0]) {
		return;
	}
	start_job_into(app, paths, len, app->paste_into, cut);
	if (cut) {
		fui_clipboard_clear(app->ui);
	}
	app->paste_into[0] = '\0';
}

static void duplicate(struct app *app) {
	size_t len;
	char **paths = selected_paths(app, &len);
	if (len) {
		start_job(app, paths, len, false);
	}
	free_paths(paths, len);
}

static void new_folder(struct app *app) {
	char name[NAME_MAX + 1];
	const char *error;
	if (in_trash(app)) {
		return;
	}
	if (!ops_new_folder(app->folder.path, name, sizeof(name), &error)) {
		toast(app, "Can't make a folder: %s", error);
		return;
	}
	snprintf(app->select_next, sizeof(app->select_next), "%s", name);
	app->rename_next = true;
	relist(app);
}

static void start_rename(struct app *app) {
	if (selected_count(app) != 1 || in_trash(app)) {
		return;
	}
	for (size_t i = 0; i < app->shown_len; i++) {
		if (app->selected[app->shown[i]]) {
			const char *name = file_at(app, i)->name;
			app->renaming = true;
			snprintf(app->rename_from, sizeof(app->rename_from), "%s", name);
			snprintf(app->rename_text, sizeof(app->rename_text), "%s", name);
			fui_focus(app->ui, fui_id("rename", 0), app->rename_text);
			app->cursor = i;
			app->has_cursor = true;
		}
	}
}

static void finish_rename(struct app *app) {
	app->renaming = false;
	const char *error;
	if (!ops_rename(app->folder.path, app->rename_from, app->rename_text, &error)) {
		toast(app, "Can't rename it: %s", error);
		return;
	}
	snprintf(app->select_next, sizeof(app->select_next), "%s", app->rename_text);
	relist(app);
}

static void trash_selected(struct app *app) {
	size_t len;
	char **paths = selected_paths(app, &len);
	if (!len) {
		free_paths(paths, len);
		return;
	}
	if (in_trash(app)) {
		app->confirming = true; // gone for good: asked first
		free_paths(paths, len);
		return;
	}
	size_t done = 0;
	for (size_t i = 0; i < len; i++) {
		const char *error;
		if (ops_trash(paths[i], &error)) {
			done++;
		} else {
			const char *slash = strrchr(paths[i], '/');
			char what[NAME_MAX + 200];
			snprintf(what, sizeof(what), "%s: %s", slash ? slash + 1 : paths[i], error);
			toast(app, "Can't move %s to the trash", what);
		}
	}
	free_paths(paths, len);
	if (done) {
		relist(app);
	}
}

static void delete_selected(struct app *app) {
	size_t len;
	char **paths = selected_paths(app, &len);
	for (size_t i = 0; i < len; i++) {
		const char *error;
		if (!ops_delete(paths[i], &error)) {
			toast(app, "Can't delete it: %s", error);
		}
	}
	free_paths(paths, len);
	relist(app);
}

static void put_back(struct app *app) {
	for (size_t i = 0; i < app->shown_len; i++) {
		const char *error;
		if (app->selected[app->shown[i]] && !ops_put_back(file_at(app, i)->name, &error)) {
			toast(app, "Can't put it back: %s", error);
		}
	}
	relist(app);
}

// ---- selection ----

static void select_only(struct app *app, size_t i) {
	selection_clear(app);
	app->selected[app->shown[i]] = true;
	app->anchor = app->cursor = i;
	app->has_cursor = true;
}

static void select_range(struct app *app, size_t i) {
	selection_clear(app);
	size_t a = app->anchor < app->shown_len ? app->anchor : 0;
	for (size_t k = a < i ? a : i; k <= (a < i ? i : a); k++) {
		app->selected[app->shown[k]] = true;
	}
	app->cursor = i;
	app->has_cursor = true;
}

// a click on shown item i, with what's held
static void click_item(struct app *app, size_t i) {
	unsigned mods = fui_mods(app->ui);
	if (mods & FUI_CTRL) {
		app->selected[app->shown[i]] = !app->selected[app->shown[i]];
		app->anchor = app->cursor = i;
		app->has_cursor = true;
	} else if (mods & FUI_SHIFT) {
		select_range(app, i);
	} else if (!app->selected[app->shown[i]]) {
		select_only(app, i);
	} else {
		// one of several selected: they stay for a drag, a click picks it alone
		app->cursor = i;
		app->has_cursor = true;
	}
	app->press_narrow = !(mods & (FUI_CTRL | FUI_SHIFT)) && selected_count(app) > 1;
	app->press_item = (long)i + 1;
	fui_pointer(app->ui, &app->press_x, &app->press_y);
	if (fui_click_count(app->ui) == 2) {
		app->press_item = 0;
		open_file(app, i);
	}
}

// ---- layout ----

static double icon_cell_w(struct app *app) {
	int cols = (int)((VIEW_W - 2 * GRID_PAD) / CELL_W);
	app->cols = cols < 1 ? 1 : cols;
	return (VIEW_W - 2 * GRID_PAD) / (double)app->cols;
}

// item i's box in folder coords (scroll not taken off)
static void item_box(struct app *app, size_t i, double *x, double *y, double *w, double *h) {
	if (app->list_view) {
		*x = VIEW_X + 8;
		*y = LIST_HEAD + 4 + (double)i * ROW_H;
		*w = VIEW_W - 16;
		*h = ROW_H;
		return;
	}
	double cw = icon_cell_w(app);
	*x = VIEW_X + GRID_PAD + (double)(i % (size_t)app->cols) * cw;
	*y = GRID_PAD + (double)(i / (size_t)app->cols) * CELL_H;
	*w = cw;
	*h = CELL_H;
}

static double content_height(struct app *app) {
	if (app->list_view) {
		return LIST_HEAD + 8 + (double)app->shown_len * ROW_H;
	}
	icon_cell_w(app);
	size_t rows = (app->shown_len + (size_t)app->cols - 1) / (size_t)app->cols;
	return 2 * GRID_PAD + (double)rows * CELL_H;
}

static void scroll_to(struct app *app, size_t i) {
	double x, y, w, h;
	item_box(app, i, &x, &y, &w, &h);
	double view_h = VIEW_BOTTOM - VIEW_TOP;
	double top = app->list_view ? y - LIST_HEAD - 4 : y;
	if (top < app->scroll) {
		app->scroll = top;
	} else if (y + h > app->scroll + view_h) {
		app->scroll = y + h - view_h + 8;
	}
}

// ---- drag and drop ----

static void file_icon(struct app *app, const struct file *f, double x, double y, double size);

// a drag's icon: the first file's, how many on a badge
static void drag_icon(struct fui *ui, void *data) {
	struct app *app = data;
	size_t first = 0, n = 0;
	for (size_t i = 0; i < app->shown_len; i++) {
		if (app->selected[app->shown[i]]) {
			first = n ? first : i;
			n++;
		}
	}
	if (!n) {
		return;
	}
	file_icon(app, file_at(app, first), 16, 16, 64);
	if (n > 1) {
		char count[16];
		snprintf(count, sizeof(count), "%zu", n);
		double w = fmax(22, fui_text_width(ui, count, F_HEAD) + 12);
		fui_fill_rounded(ui, 82 - w / 2, 8, w, 22, 11, 0xff3b30ff);
		fui_text(ui, 82, 11, count, F_HEAD, 0xffffffff, FUI_CENTER, 0);
	}
}

// a press on a file moved far enough: the selection dragged. let go
// without: a click on one of several picks just it
static void drag_check(struct app *app) {
	struct fui *ui = app->ui;
	if (!app->press_item) {
		return;
	}
	size_t i = (size_t)(app->press_item - 1);
	if (!fui_button_held(ui)) {
		if (app->press_narrow && i < app->shown_len) {
			select_only(app, i);
		}
		app->press_item = 0;
		return;
	}
	double px, py;
	fui_pointer(ui, &px, &py);
	if (app->renaming || app->pick || hypot(px - app->press_x, py - app->press_y) < 6) {
		return;
	}
	app->press_item = 0;
	size_t len;
	char **paths = selected_paths(app, &len);
	if (len) {
		fui_drag_files(ui, paths, len, 96, 96, drag_icon, app);
	}
	free_paths(paths, len);
}

// what a drop at x, y goes into: a folder shown, a place in the sidebar,
// else the folder open. false for nowhere (a chooser takes nothing)
static bool target_at(struct app *app, double x, double y, char *dir, size_t size, bool *trash) {
	*trash = false;
	app->drop_item = 0;
	app->drop_place = -1;
	app->drop_here = false;
	if (app->pick) {
		return false;
	}
	if (x < SIDEBAR_W) {
		for (int i = 0; i < app->places_len; i++) {
			if (y >= app->place_y[i] && y < app->place_y[i] + 28 && x >= 10) {
				*trash = app->places[i].icon == PLACE_TRASH;
				snprintf(dir, size, "%s", app->places[i].path);
				app->drop_place = i;
				return true;
			}
		}
		return false;
	}
	if (y < VIEW_TOP || y >= VIEW_BOTTOM) {
		return false;
	}
	for (size_t i = 0; i < app->shown_len; i++) {
		const struct file *f = file_at(app, i);
		double bx, by, bw, bh;
		item_box(app, i, &bx, &by, &bw, &bh);
		by += app->view_top;
		bool on;
		if (app->list_view) {
			on = x >= bx && x < bx + bw && y >= by && y < by + bh;
		} else {
			// the icon and its name, like a click
			double ix = bx + (bw - ICON) / 2, iy = by + 6;
			on = (x >= ix - 8 && x < ix + ICON + 8 && y >= iy - 6 && y < iy + ICON + 4)
				|| (x >= bx + 4 && x < bx + bw - 4 && y >= iy + ICON + 7 && y < iy + ICON + 43);
		}
		if (on && f->kind == KIND_FOLDER) {
			path_of(app, f->name, dir, size);
			app->drop_item = (long)i + 1;
			return true;
		}
	}
	*trash = in_trash(app);
	snprintf(dir, size, "%s", app->folder.path);
	app->drop_here = true;
	return true;
}

// a drop into dir: moved to the trash, moved on the same drive, else
// copied (ctrl: copied anyway). nothing for a folder onto itself or files
// where they are
static enum fui_dnd_action drop_action(struct app *app, const char *dir, bool trash) {
	if (access(dir, W_OK) < 0) {
		return FUI_DND_NONE;
	}
	if (trash) {
		return FUI_DND_MOVE;
	}
	// whose files, from where: read off the drag itself (another window's,
	// another app's), else not known yet
	size_t len;
	char **files = fui_dnd_files(app->ui, &len);
	if (!files || !len) {
		return FUI_DND_COPY;
	}
	bool all_here = true;
	size_t dl = strlen(dir);
	for (size_t i = 0; i < len; i++) {
		const char *p = files[i];
		size_t pl = strlen(p);
		if (!strncmp(dir, p, pl) && (dir[pl] == '/' || !dir[pl])) {
			return FUI_DND_NONE; // into itself
		}
		const char *slash = strrchr(p, '/');
		size_t parent = slash ? (size_t)(slash - p) : 0;
		all_here &= parent == dl ? !strncmp(p, dir, dl) : (parent == 0 && !strcmp(dir, "/"));
	}
	if (all_here) {
		return FUI_DND_NONE;
	}
	if (fui_mods(app->ui) & FUI_CTRL) {
		return FUI_DND_COPY;
	}
	struct stat a, b;
	bool same_drive = stat(dir, &a) == 0 && stat(files[0], &b) == 0 && a.st_dev == b.st_dev;
	return same_drive ? FUI_DND_MOVE : FUI_DND_COPY;
}

// a drag over the window: where it'd go lit, what it'd do said. a drop:
// done
static void dnd(struct app *app) {
	struct fui *ui = app->ui;
	char dir[PATH_MAX];
	bool trash;
	size_t len;
	enum fui_dnd_action action;
	double x, y;
	char **paths = fui_dnd_dropped(ui, &len, &action, &x, &y);
	if (paths) {
		if (target_at(app, x, y, dir, sizeof(dir), &trash) && action != FUI_DND_NONE) {
			if (trash) {
				for (size_t i = 0; i < len; i++) {
					const char *error;
					if (!ops_trash(paths[i], &error)) {
						toast(app, "Can't move it to the trash: %s", error);
					}
				}
				relist(app);
			} else {
				start_job_into(app, paths, len, dir, action == FUI_DND_MOVE);
			}
		}
		app->drop_item = 0;
		app->drop_place = -1;
		app->drop_here = false;
		return;
	}
	if (fui_dnd_over(ui, &x, &y)) {
		bool there = target_at(app, x, y, dir, sizeof(dir), &trash);
		fui_dnd_accept(ui, there ? drop_action(app, dir, trash) : FUI_DND_NONE);
	} else {
		app->drop_item = 0;
		app->drop_place = -1;
		app->drop_here = false;
	}
}

// ---- zoom, and what's kept of it ----

static void zoom(struct app *app, double by) {
	if (app->list_view) {
		app->row_want = fmax(20, fmin(app->row_want * by, 48));
	} else {
		app->icon_want = fmax(32, fmin(app->icon_want * by, 256));
	}
	fui_redraw(app->ui);
}

static void zoom_reset(struct app *app) {
	if (app->list_view) {
		app->row_want = ROW_DEFAULT;
	} else {
		app->icon_want = ICON_DEFAULT;
	}
	fui_redraw(app->ui);
}

// how it's shown, kept from one run to the next: $XDG_CONFIG_HOME/flame/files.conf
static void prefs_path(char *out, size_t size) {
	const char *config = getenv("XDG_CONFIG_HOME");
	if (config && *config == '/') {
		snprintf(out, size, "%s/flame/files.conf", config);
	} else {
		snprintf(out, size, "%s/.config/flame/files.conf", home_dir());
	}
}

static void prefs_load(struct app *app) {
	char path[PATH_MAX], line[128];
	prefs_path(path, sizeof(path));
	FILE *f = fopen(path, "re");
	while (f && fgets(line, sizeof(line), f)) {
		double v;
		if (sscanf(line, "icon=%lf", &v) == 1) {
			app->icon_want = fmax(32, fmin(v, 256));
		} else if (sscanf(line, "row=%lf", &v) == 1) {
			app->row_want = fmax(20, fmin(v, 48));
		} else if (!strncmp(line, "view=list", 9)) {
			app->list_view = true;
		} else if (!strncmp(line, "hidden=1", 8)) {
			app->hidden = true;
		}
	}
	if (f) {
		fclose(f);
	}
}

static void prefs_save(struct app *app) {
	char path[PATH_MAX];
	prefs_path(path, sizeof(path));
	char *slash = strrchr(path, '/');
	if (slash) {
		*slash = '\0';
		mkdir(path, 0755);
		*slash = '/';
	}
	FILE *f = fopen(path, "we");
	if (f) {
		fprintf(f, "view=%s\nicon=%.0f\nrow=%.0f\nhidden=%d\n", app->list_view ? "list" : "icons",
			app->icon_want, app->row_want, app->hidden);
		fclose(f);
	}
}

// ---- keys ----

static void move_cursor(struct app *app, long by) {
	if (!app->shown_len) {
		return;
	}
	long at = app->has_cursor ? (long)app->cursor + by : by > 0 ? 0 : (long)app->shown_len - 1;
	at = at < 0 ? 0 : at >= (long)app->shown_len ? (long)app->shown_len - 1 : at;
	if (fui_mods(app->ui) & FUI_SHIFT) {
		select_range(app, (size_t)at);
	} else {
		select_only(app, (size_t)at);
	}
	scroll_to(app, (size_t)at);
}

static void keys(struct app *app) {
	struct fui *ui = app->ui;
	if (app->confirming) {
		return; // the question takes them
	}
	unsigned shift = FUI_SHIFT;
	long down = app->list_view ? 1 : app->cols;
	if (fui_key_pressed(ui, XKB_KEY_Down, 0) || fui_key_pressed(ui, XKB_KEY_Down, shift)) {
		move_cursor(app, down);
	}
	if (fui_key_pressed(ui, XKB_KEY_Up, 0) || fui_key_pressed(ui, XKB_KEY_Up, shift)) {
		move_cursor(app, -down);
	}
	if (!app->list_view && (fui_key_pressed(ui, XKB_KEY_Right, 0)
			|| fui_key_pressed(ui, XKB_KEY_Right, shift))) {
		move_cursor(app, 1);
	}
	if (!app->list_view && (fui_key_pressed(ui, XKB_KEY_Left, 0)
			|| fui_key_pressed(ui, XKB_KEY_Left, shift))) {
		move_cursor(app, -1);
	}
	if (fui_key_pressed(ui, XKB_KEY_Home, 0) && app->shown_len) {
		select_only(app, 0);
		scroll_to(app, 0);
	}
	if (fui_key_pressed(ui, XKB_KEY_End, 0) && app->shown_len) {
		select_only(app, app->shown_len - 1);
		scroll_to(app, app->shown_len - 1);
	}
	if (fui_key_pressed(ui, XKB_KEY_Return, 0) || fui_key_pressed(ui, XKB_KEY_KP_Enter, 0)
			|| fui_key_pressed(ui, XKB_KEY_Down, FUI_CTRL)) {
		if (app->has_cursor && app->selected[app->shown[app->cursor]]) {
			open_file(app, app->cursor);
			return;
		}
	}
	if (fui_key_pressed(ui, XKB_KEY_BackSpace, 0) || fui_key_pressed(ui, XKB_KEY_Up, FUI_ALT)
			|| fui_key_pressed(ui, XKB_KEY_Up, FUI_CTRL)) {
		go_up(app);
		return;
	}
	if (fui_key_pressed(ui, XKB_KEY_Left, FUI_ALT) || fui_key_pressed(ui, XKB_KEY_bracketleft,
			FUI_CTRL)) {
		history(app, false);
		return;
	}
	if (fui_key_pressed(ui, XKB_KEY_Right, FUI_ALT) || fui_key_pressed(ui, XKB_KEY_bracketright,
			FUI_CTRL)) {
		history(app, true);
		return;
	}
	if (fui_key_pressed(ui, XKB_KEY_a, FUI_CTRL)) {
		for (size_t i = 0; i < app->shown_len; i++) {
			app->selected[app->shown[i]] = true;
		}
	}
	if (fui_key_pressed(ui, XKB_KEY_Escape, 0)) {
		selection_clear(app);
		app->banding = false;
		if (app->pick) {
			app->status = 1;
			fui_quit(ui);
		}
	}
	if (fui_key_pressed(ui, XKB_KEY_h, FUI_CTRL)) {
		app->hidden = !app->hidden;
		rebuild(app);
	}
	if (fui_key_pressed(ui, XKB_KEY_1, FUI_CTRL)) {
		app->list_view = false;
	}
	if (fui_key_pressed(ui, XKB_KEY_2, FUI_CTRL)) {
		app->list_view = true;
	}
	if (fui_key_pressed(ui, XKB_KEY_equal, FUI_CTRL) || fui_key_pressed(ui, XKB_KEY_plus,
			FUI_CTRL | FUI_SHIFT) || fui_key_pressed(ui, XKB_KEY_plus, FUI_CTRL)
			|| fui_key_pressed(ui, XKB_KEY_KP_Add, FUI_CTRL)) {
		zoom(app, 1.25);
	}
	if (fui_key_pressed(ui, XKB_KEY_minus, FUI_CTRL) || fui_key_pressed(ui, XKB_KEY_KP_Subtract,
			FUI_CTRL)) {
		zoom(app, 0.8);
	}
	if (fui_key_pressed(ui, XKB_KEY_0, FUI_CTRL) || fui_key_pressed(ui, XKB_KEY_KP_0, FUI_CTRL)) {
		zoom_reset(app);
	}
	if (fui_key_pressed(ui, XKB_KEY_f, FUI_CTRL)) {
		fui_focus(ui, fui_id("search", 0), app->query);
	}
	if (fui_key_pressed(ui, XKB_KEY_l, FUI_CTRL) || fui_key_pressed(ui, XKB_KEY_r, FUI_CTRL)) {
		relist(app);
	}
	if (app->pick) {
		return; // a chooser changes nothing
	}
	if (fui_key_pressed(ui, XKB_KEY_F2, 0)) {
		start_rename(app);
	}
	if (fui_key_pressed(ui, XKB_KEY_Delete, 0)) {
		trash_selected(app);
	}
	if (fui_key_pressed(ui, XKB_KEY_Delete, FUI_SHIFT)) {
		app->confirming = selected_count(app) > 0;
	}
	if (fui_key_pressed(ui, XKB_KEY_c, FUI_CTRL)) {
		clip_set(app, false);
	}
	if (fui_key_pressed(ui, XKB_KEY_c, FUI_CTRL | FUI_SHIFT)) {
		copy_paths(app);
	}
	if (fui_key_pressed(ui, XKB_KEY_x, FUI_CTRL)) {
		clip_set(app, true);
	}
	if (fui_key_pressed(ui, XKB_KEY_v, FUI_CTRL)) {
		paste(app);
	}
	if (fui_key_pressed(ui, XKB_KEY_d, FUI_CTRL)) {
		duplicate(app);
	}
	if (fui_key_pressed(ui, XKB_KEY_n, FUI_CTRL | FUI_SHIFT)) {
		new_folder(app);
	}
}

// a name's first letters typed: it's selected
static void typeahead(struct app *app) {
	const char *typed = fui_typed(app->ui);
	if (!*typed || app->confirming) {
		return;
	}
	double now = fui_time(app->ui);
	if (now - app->typeahead_at > 1.0) {
		app->typeahead[0] = '\0';
	}
	app->typeahead_at = now;
	size_t len = strlen(app->typeahead);
	snprintf(app->typeahead + len, sizeof(app->typeahead) - len, "%s", typed);
	for (size_t i = 0; i < app->shown_len; i++) {
		if (!strncasecmp(file_at(app, i)->name, app->typeahead, strlen(app->typeahead))) {
			select_only(app, i);
			scroll_to(app, i);
			return;
		}
	}
}

// ---- drawing ----

static void chevron(struct fui *ui, double cx, double cy, bool left, uint32_t color) {
	cairo_t *cr = fui_cairo(ui);
	fui_color(ui, color);
	cairo_set_line_width(cr, 1.8);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	double d = left ? 1 : -1;
	cairo_move_to(cr, cx + 2.5 * d, cy - 5);
	cairo_line_to(cr, cx - 2.5 * d, cy);
	cairo_line_to(cr, cx + 2.5 * d, cy + 5);
	cairo_stroke(cr);
}

// a small square button with a glyph drawn by f, true when clicked
static bool tool_button(struct fui *ui, uint64_t id, double x, double y, bool enabled, bool on) {
	bool hover = enabled && fui_hover(ui, x, y, 30, 28);
	double lit = fui_ease(ui, id, hover ? 1 : 0, 20);
	if (on || lit > 0.01) {
		fui_fill_rounded(ui, x, y, 30, 28, 7, on ? 0xffffff24
			: (uint32_t)(0xffffff00 | (uint32_t)(0x16 * lit)));
	}
	if (hover) {
		fui_cursor_pointer(ui);
	}
	return enabled && fui_clicked(ui, x, y, 30, 28);
}

static void sidebar(struct app *app) {
	struct fui *ui = app->ui;
	double y = 18;
	const double x = 10, w = SIDEBAR_W - 20;
	bool locations = false;
	fui_text(ui, x + 8, y, "Favorites", F_HEAD, TEXT_FAINT, FUI_LEFT, 0);
	y += 20;
	for (int i = 0; i < app->places_len; i++) {
		const struct place_item *p = &app->places[i];
		if (p->location && !locations) {
			locations = true;
			y += 12;
			fui_text(ui, x + 8, y, "Locations", F_HEAD, TEXT_FAINT, FUI_LEFT, 0);
			y += 20;
		}
		bool here = !strcmp(app->folder.path, p->path);
		bool hover = fui_hover(ui, x, y, w, 28);
		app->place_y[i] = y;
		if (app->drop_place == i) {
			fui_fill_rounded(ui, x, y, w, 28, 7, 0xff7a2a66);
		}
		double lit = fui_ease(ui, fui_id(p->path, 3), hover && !here ? 1 : 0, 20);
		if (here) {
			fui_fill_rounded(ui, x, y, w, 28, 7, 0xffffff26);
		} else if (lit > 0.01) {
			fui_fill_rounded(ui, x, y, w, 28, 7, (uint32_t)(0xffffff00 | (uint32_t)(0x12 * lit)));
		}
		if (hover) {
			fui_cursor_pointer(ui);
		}
		if (fui_clicked(ui, x, y, w, 28) && !here) {
			go(app, p->path, true);
		}
		icon_place(ui, p->icon, x + 9, y + 6, 16, ACCENT);
		fui_text(ui, x + 34, y + 6, p->name, F_MEDIUM, TEXT, FUI_LEFT, w - 40);
		y += 30;
	}
}

static void toolbar(struct app *app) {
	struct fui *ui = app->ui;
	double x = VIEW_X + 12, y = 12;
	if (tool_button(ui, fui_id("back", 0), x, y, app->back_len > 0, false)) {
		history(app, false);
	}
	chevron(ui, x + 15, y + 14, true, app->back_len ? TEXT : TEXT_FAINT);
	x += 32;
	if (tool_button(ui, fui_id("forward", 0), x, y, app->forward_len > 0, false)) {
		history(app, true);
	}
	chevron(ui, x + 15, y + 14, false, app->forward_len ? TEXT : TEXT_FAINT);
	x += 42;
	// where it is: the place's name, else the folder's
	const char *title = app->folder.path;
	for (int i = 0; i < app->places_len; i++) {
		if (!strcmp(app->places[i].path, app->folder.path)) {
			title = app->places[i].name;
		}
	}
	const char *slash = strrchr(title, '/');
	if (title == app->folder.path && slash && slash[1]) {
		title = slash + 1;
	}
	double right = W - 12;
	// the search, then views (and the trash's button) left of it
	const double sw = W > 860 ? 200 : 150;
	right -= sw;
	fui_search(ui, fui_id("search", 0), right, y, sw, app->query, sizeof(app->query), false);
	if (strcmp(app->searched, app->query)) {
		snprintf(app->searched, sizeof(app->searched), "%s", app->query);
		rebuild(app);
		app->scroll = 0;
	}
	right -= 72;
	if (tool_button(ui, fui_id("icons", 0), right, y, true, !app->list_view)) {
		app->list_view = false;
	}
	// four squares
	for (int i = 0; i < 4; i++) {
		fui_fill_rounded(ui, right + 9 + (i % 2) * 7, y + 7 + (i / 2) * 7, 5.5, 5.5, 1.5, TEXT);
	}
	if (tool_button(ui, fui_id("list", 0), right + 32, y, true, app->list_view)) {
		app->list_view = true;
	}
	for (int i = 0; i < 3; i++) {
		fui_fill_rounded(ui, right + 41, y + 8 + i * 5, 2.5, 2.5, 1.25, TEXT);
		fui_fill_rounded(ui, right + 46, y + 8.5 + i * 5, 9, 1.6, 0.8, TEXT);
	}
	if (in_trash(app) && !app->pick) {
		double bw = fui_text_width(ui, "Empty", F_MEDIUM) + 28;
		right -= bw + 12;
		if (fui_button(ui, fui_id("empty trash", 0), right, y, "Empty") && app->folder.len) {
			selection_clear(app);
			for (size_t i = 0; i < app->shown_len; i++) {
				app->selected[app->shown[i]] = true;
			}
			app->confirming = true;
		}
	}
	fui_text(ui, x, y + 5, title, F_TITLE, TEXT, FUI_LEFT, right - x - 12);
}

// a file's icon in a box size big
static void file_icon(struct app *app, const struct file *f, double x, double y, double size) {
	char path[PATH_MAX];
	path_of(app, f->name, path, sizeof(path));
	if (f->kind == KIND_FOLDER) {
		icon_folder(app->ui, x, y, size, place_of(app, path));
		return;
	}
	// a picture of what's in it: the picture, a video's frame, a song's cover
	if (f->kind == KIND_IMAGE || f->kind == KIND_VIDEO || f->kind == KIND_AUDIO) {
		bool failed;
		// a few sizes only: zooming doesn't read every picture again at every step
		int want = size <= 24 ? 48 : size <= 48 ? 96 : size <= 96 ? 192 : 384;
		cairo_surface_t *thumb = fui_thumb_fit(app->ui, path, want, &failed);
		if (thumb) {
			icon_picture(app->ui, x, y, size, thumb);
			return;
		}
	}
	icon_file(app->ui, x, y, size, f->kind, f->name);
}

static void rename_field(struct app *app, double x, double y, double w) {
	if (fui_field(app->ui, fui_id("rename", 0), x, y, w, app->rename_text,
			sizeof(app->rename_text), "", true)) {
		finish_rename(app);
	} else if (!fui_typing(app->ui)) {
		app->renaming = false; // escape, or clicked away unchanged
	}
}

static void icon_view(struct app *app, double top) {
	struct fui *ui = app->ui;
	double cw = icon_cell_w(app);
	for (size_t i = 0; i < app->shown_len; i++) {
		const struct file *f = file_at(app, i);
		double x, y, w, h;
		item_box(app, i, &x, &y, &w, &h);
		y += top;
		if (y + h < VIEW_TOP || y > VIEW_BOTTOM) {
			continue; // off screen
		}
		bool sel = app->selected[app->shown[i]];
		bool dim = clipped(app, f->name) || (app->pick && f->kind != KIND_FOLDER
			&& !pickable(app, f));
		cairo_t *cr = fui_cairo(ui);
		cairo_push_group(cr);
		double ix = x + (cw - ICON) / 2, iy = y + 6;
		if (app->drop_item == (long)i + 1) {
			// what a drop goes into
			fui_fill_rounded(ui, ix - 7, iy - 5, ICON + 14, ICON + 10, 8, 0xff7a2a59);
			fui_rounded(ui, ix - 6, iy - 4, ICON + 12, ICON + 8, 7);
			fui_color(ui, ACCENT);
			cairo_set_line_width(fui_cairo(ui), 2);
			cairo_stroke(fui_cairo(ui));
		} else if (sel) {
			fui_fill_rounded(ui, ix - 7, iy - 5, ICON + 14, ICON + 10, 8, 0xffffff1c);
		}
		file_icon(app, f, ix, iy, ICON);
		double ny = iy + ICON + 9;
		bool renaming_this = app->renaming && !strcmp(f->name, app->rename_from);
		if (renaming_this) {
			rename_field(app, x + 4, ny - 4, cw - 8);
		} else {
			double tw = fmin(fui_text_width(ui, f->name, F_TEXT) + 10, cw - 8);
			if (sel) {
				double th = fui_text_width(ui, f->name, F_TEXT) + 10 > cw - 8 ? 34 : 18;
				fui_fill_rounded(ui, x + (cw - tw) / 2, ny - 1, tw, th, 5, ACCENT);
			}
			fui_text_box(ui, x + 6, ny, cw - 12, 2, f->name, F_TEXT, sel ? 0xffffffff : TEXT,
				FUI_CENTER);
		}
		cairo_pop_group_to_source(cr);
		cairo_paint_with_alpha(cr, dim ? 0.45 : 1);
		// what's clickable: the icon and the name, not the space round them
		bool on = fui_hover(ui, ix - 8, iy - 6, ICON + 16, ICON + 10)
			|| fui_hover(ui, x + 4, ny - 2, cw - 8, 36);
		if (on && !renaming_this && fui_right_clicked(ui, x, y, cw, h)) {
			if (!sel) {
				select_only(app, i);
			}
			double px, py;
			fui_pointer(ui, &px, &py);
			fui_menu_open(ui, fui_id("items", 0), px, py);
		}
		if (on && !renaming_this && fui_clicked(ui, x, y, cw, h)) {
			click_item(app, i);
		}
	}
}

static void list_header(struct app *app, double y) {
	struct fui *ui = app->ui;
	double x = VIEW_X + 8, w = VIEW_W - 16;
	bool narrow = w < 560;
	double kind_w = narrow ? 0 : 140, size_w = 90, date_w = narrow ? 150 : 170;
	double name_w = w - kind_w - size_w - date_w;
	struct {
		const char *label;
		enum sort_key key;
		double x, w;
	} cols[] = {
		{"Name", SORT_NAME, x, name_w},
		{"Date Modified", SORT_DATE, x + name_w, date_w},
		{"Size", SORT_SIZE, x + name_w + date_w, size_w},
		{"Kind", SORT_KIND, x + name_w + date_w + size_w, kind_w},
	};
	fui_fill_rounded(ui, VIEW_X, y, VIEW_W, LIST_HEAD, 0, CONTENT_BG);
	fui_fill_rounded(ui, x, y + LIST_HEAD - 1, w, 1, 0, 0xffffff14);
	for (int c = 0; c < 4; c++) {
		if (cols[c].w <= 0) {
			continue;
		}
		bool hover = fui_hover(ui, cols[c].x, y, cols[c].w, LIST_HEAD);
		bool sorted = app->sort == cols[c].key;
		if (hover) {
			fui_cursor_pointer(ui);
		}
		if (fui_clicked(ui, cols[c].x, y, cols[c].w, LIST_HEAD)) {
			app->descending = sorted ? !app->descending : false;
			app->sort = cols[c].key;
			rebuild(app);
		}
		double tw = fui_text(ui, cols[c].x + 8, y + 7, cols[c].label, F_HEAD,
			sorted ? TEXT : TEXT_DIM, FUI_LEFT, cols[c].w - 24);
		if (sorted) {
			cairo_t *cr = fui_cairo(ui);
			double cx = cols[c].x + 8 + tw + 8, cy = y + 14;
			double d = app->descending ? -1 : 1;
			fui_color(ui, TEXT_DIM);
			cairo_set_line_width(cr, 1.4);
			cairo_move_to(cr, cx - 3, cy - 1.5 * d);
			cairo_line_to(cr, cx, cy + 1.5 * d);
			cairo_line_to(cr, cx + 3, cy - 1.5 * d);
			cairo_stroke(cr);
		}
	}
}

static void list_view(struct app *app, double top) {
	struct fui *ui = app->ui;
	// the header stays put over the rows: it takes its clicks first
	cairo_push_group(fui_cairo(ui));
	list_header(app, VIEW_TOP);
	cairo_pattern_t *header = cairo_pop_group(fui_cairo(ui));
	double x0 = VIEW_X + 8, w = VIEW_W - 16;
	bool narrow = w < 560;
	double kind_w = narrow ? 0 : 140, size_w = 90, date_w = narrow ? 150 : 170;
	double name_w = w - kind_w - size_w - date_w;
	for (size_t i = 0; i < app->shown_len; i++) {
		const struct file *f = file_at(app, i);
		double x, y, rw, h;
		item_box(app, i, &x, &y, &rw, &h);
		y += top;
		if (y + h < VIEW_TOP + LIST_HEAD || y > VIEW_BOTTOM) {
			continue;
		}
		bool sel = app->selected[app->shown[i]];
		bool dim = clipped(app, f->name) || (app->pick && f->kind != KIND_FOLDER
			&& !pickable(app, f));
		if (app->drop_item == (long)i + 1) {
			fui_fill_rounded(ui, x, y, rw, h, 6, 0xff7a2a59);
			fui_rounded(ui, x + 1, y + 1, rw - 2, h - 2, 5);
			fui_color(ui, ACCENT);
			cairo_set_line_width(fui_cairo(ui), 2);
			cairo_stroke(fui_cairo(ui));
		} else if (sel) {
			fui_fill_rounded(ui, x, y, rw, h, 6, ACCENT);
		} else if (i % 2) {
			fui_fill_rounded(ui, x, y, rw, h, 6, 0xffffff06);
		}
		cairo_t *cr = fui_cairo(ui);
		cairo_push_group(cr);
		double is = ROW_H - 6, ty = y + (ROW_H - 15) / 2;
		file_icon(app, f, x0 + 6, y + 3, is);
		bool renaming_this = app->renaming && !strcmp(f->name, app->rename_from);
		if (renaming_this) {
			rename_field(app, x0 + is + 12, y + (ROW_H - 28) / 2, name_w - is - 22);
		} else {
			fui_text(ui, x0 + is + 14, ty, f->name, F_TEXT, sel ? 0xffffffff : TEXT, FUI_LEFT,
				name_w - is - 22);
		}
		uint32_t dimmed = sel ? 0xffffffd0 : TEXT_DIM;
		char text[64];
		format_date(f->mtime, text, sizeof(text));
		fui_text(ui, x0 + name_w + 8, ty, text, F_TEXT, dimmed, FUI_LEFT, date_w - 16);
		format_size(f, text, sizeof(text));
		fui_text(ui, x0 + name_w + date_w + 8, ty, text, F_TEXT, dimmed, FUI_LEFT, size_w - 16);
		if (kind_w > 0) {
			fui_text(ui, x0 + name_w + date_w + size_w + 8, ty, f->kind_name, F_TEXT, dimmed,
				FUI_LEFT, kind_w - 16);
		}
		cairo_pop_group_to_source(cr);
		cairo_paint_with_alpha(cr, dim ? 0.45 : 1);
		bool under_header = fui_hover(ui, VIEW_X, VIEW_TOP, VIEW_W, LIST_HEAD);
		if (!renaming_this && !under_header && fui_right_clicked(ui, x, y, rw, h)) {
			if (!sel) {
				select_only(app, i);
			}
			double px, py;
			fui_pointer(ui, &px, &py);
			fui_menu_open(ui, fui_id("items", 0), px, py);
		}
		if (!renaming_this && !under_header && fui_clicked(ui, x, y, rw, h)) {
			click_item(app, i);
		}
	}
	cairo_set_source(fui_cairo(ui), header);
	cairo_paint(fui_cairo(ui));
	cairo_pattern_destroy(header);
}

// a box dragged over the files picks what it touches
static void band(struct app *app, double top) {
	struct fui *ui = app->ui;
	double px, py;
	fui_pointer(ui, &px, &py);
	if (!fui_button_held(ui)) {
		app->banding = false;
		return;
	}
	double bx = fmin(app->band_x, px), by = fmin(app->band_y, py - top);
	double bw = fabs(px - app->band_x), bh = fabs(py - top - app->band_y);
	for (size_t i = 0; i < app->shown_len; i++) {
		double x, y, w, h;
		item_box(app, i, &x, &y, &w, &h);
		if (!app->list_view) {
			x += (w - ICON) / 2 - 6;
			w = ICON + 12;
		}
		app->selected[app->shown[i]] = x < bx + bw && x + w > bx && y < by + bh && y + h > by;
	}
	fui_fill_rounded(ui, bx, by + top, bw, bh, 2, 0xffffff12);
	fui_rounded(ui, bx + 0.5, by + top + 0.5, bw, bh, 2);
	fui_color(ui, 0xffffff50);
	cairo_set_line_width(fui_cairo(ui), 1);
	cairo_stroke(fui_cairo(ui));
	// near the edge: it scrolls
	if (py > VIEW_BOTTOM - 20) {
		app->scroll += 8;
		fui_redraw(ui);
	} else if (py < VIEW_TOP + 20 && app->scroll > 0) {
		app->scroll -= 8;
		fui_redraw(ui);
	}
}

static void view(struct app *app) {
	struct fui *ui = app->ui;
	cairo_t *cr = fui_cairo(ui);
	double view_h = VIEW_BOTTOM - VIEW_TOP;
	if (fui_hover(ui, VIEW_X, VIEW_TOP, VIEW_W, view_h)) {
		double wheel = fui_scrolled(ui);
		if (fui_mods(ui) & FUI_CTRL) {
			// ctrl and the wheel: bigger up, smaller down
			if (wheel != 0) {
				zoom(app, pow(1.015, -wheel));
			}
		} else {
			app->scroll += wheel;
		}
	}
	double most = content_height(app) - view_h;
	app->scroll = fmax(0, fmin(app->scroll, fmax(most, 0)));
	double eased = fui_ease(ui, fui_id("scroll", 0), app->scroll, 26);
	double top = VIEW_TOP - eased;
	app->view_top = top;
	cairo_save(cr);
	cairo_rectangle(cr, VIEW_X, VIEW_TOP, VIEW_W, view_h);
	cairo_clip(cr);
	if (app->list_view) {
		list_view(app, top);
	} else {
		icon_view(app, top);
	}
	if (!app->shown_len) {
		fui_text(ui, VIEW_X + VIEW_W / 2, VIEW_TOP + view_h / 2 - 10, app->query[0]
			? "Nothing matches" : in_trash(app) ? "The trash is empty" : "This folder is empty",
			F_MEDIUM, TEXT_FAINT, FUI_CENTER, 0);
	}
	if (app->banding) {
		band(app, top);
	}
	if (app->drop_here) {
		// the folder open is where a drop goes
		fui_rounded(ui, VIEW_X + 4, VIEW_TOP + 2, VIEW_W - 8, view_h - 4, 10);
		fui_color(ui, 0xff7a2acc);
		cairo_set_line_width(cr, 2);
		cairo_stroke(cr);
	}
	cairo_restore(cr);
	// what's left of a click in the view: the empty space between files
	bool over = fui_hover(ui, VIEW_X, VIEW_TOP, VIEW_W, view_h);
	if (over && fui_right_clicked(ui, VIEW_X, VIEW_TOP, VIEW_W, view_h)) {
		selection_clear(app);
		double px, py;
		fui_pointer(ui, &px, &py);
		fui_menu_open(ui, fui_id("empty", 0), px, py);
	}
	if (over && fui_clicked(ui, VIEW_X, VIEW_TOP, VIEW_W, view_h)) {
		if (!(fui_mods(ui) & (FUI_CTRL | FUI_SHIFT))) {
			selection_clear(app);
		}
		double px, py;
		fui_pointer(ui, &px, &py);
		app->banding = true;
		app->band_x = px;
		app->band_y = py - top;
	}
}

static void status_bar(struct app *app) {
	struct fui *ui = app->ui;
	double y = VIEW_BOTTOM;
	fui_fill_rounded(ui, VIEW_X, y, VIEW_W, 1, 0, 0xffffff12);
	char text[128];
	size_t sel = selected_count(app);
	if (sel) {
		snprintf(text, sizeof(text), "%zu of %zu selected", sel, app->shown_len);
	} else {
		snprintf(text, sizeof(text), "%zu item%s", app->shown_len, app->shown_len == 1 ? "" : "s");
	}
	struct statvfs vfs;
	if (statvfs(app->folder.path, &vfs) == 0) {
		struct file free_space = {.size = (off_t)(vfs.f_bavail * vfs.f_frsize), .kind = KIND_OTHER};
		char amount[32];
		format_size(&free_space, amount, sizeof(amount));
		size_t len = strlen(text);
		snprintf(text + len, sizeof(text) - len, ", %s available", amount);
	}
	if (app->pick) {
		// a chooser: what to do with the pick
		double bx = W - 12;
		const char *ok = app->pick_folder ? "Choose" : "Open";
		bool can = false;
		size_t at = 0;
		for (size_t i = 0; i < app->shown_len; i++) {
			if (app->selected[app->shown[i]] && pickable(app, file_at(app, i))) {
				can = true;
				at = i;
			}
		}
		can = can || (app->pick_folder && !sel);
		bx -= fui_text_width(ui, ok, F_MEDIUM) + 28;
		if (fui_button(ui, fui_id("pick ok", 0), bx, y + 1, ok) && can) {
			char path[PATH_MAX];
			if (sel) {
				path_of(app, file_at(app, at)->name, path, sizeof(path));
			} else {
				snprintf(path, sizeof(path), "%s", app->folder.path);
			}
			picked(app, path);
		}
		bx -= fui_text_width(ui, "Cancel", F_MEDIUM) + 28 + 8;
		if (fui_button(ui, fui_id("pick cancel", 0), bx, y + 1, "Cancel")) {
			app->status = 1;
			fui_quit(ui);
		}
		fui_text(ui, VIEW_X + 14, y + 8, text, F_SMALL, TEXT_DIM, FUI_LEFT, bx - VIEW_X - 24);
		return;
	}
	if (app->job) {
		struct ops_job *job = app->job;
		pthread_mutex_lock(&job->lock);
		double frac = job->total ? (double)job->done_bytes / (double)job->total : 0;
		char what[NAME_MAX + 32];
		snprintf(what, sizeof(what), "%s %s", job->move ? "Moving" : "Copying", job->current);
		pthread_mutex_unlock(&job->lock);
		double bw = 140, bx = W - 14 - bw - 24;
		fui_fill_rounded(ui, bx, y + 12, bw, 6, 3, 0xffffff1a);
		fui_fill_rounded(ui, bx, y + 12, fmax(6, bw * fmin(frac, 1)), 6, 3, ACCENT);
		fui_text(ui, bx - 10, y + 8, what, F_SMALL, TEXT_DIM, FUI_RIGHT, bx - VIEW_X - 220);
		// stop it
		double cx = W - 24, cy = y + 15;
		bool hover = fui_hover(ui, cx - 8, cy - 8, 16, 16);
		cairo_t *cr = fui_cairo(ui);
		cairo_arc(cr, cx, cy, 7, 0, 2 * M_PI);
		fui_color(ui, hover ? 0xffffff66 : 0xffffff33);
		cairo_fill(cr);
		fui_color(ui, CONTENT_BG);
		cairo_set_line_width(cr, 1.4);
		cairo_move_to(cr, cx - 2.5, cy - 2.5);
		cairo_line_to(cr, cx + 2.5, cy + 2.5);
		cairo_move_to(cr, cx + 2.5, cy - 2.5);
		cairo_line_to(cr, cx - 2.5, cy + 2.5);
		cairo_stroke(cr);
		if (fui_clicked(ui, cx - 8, cy - 8, 16, 16)) {
			pthread_mutex_lock(&job->lock);
			job->cancel = true;
			pthread_mutex_unlock(&job->lock);
		}
	}
	fui_text(ui, VIEW_X + 14, y + 8, text, F_SMALL, TEXT_DIM, FUI_LEFT, VIEW_W / 2);
}

// a job finished: what it made selected, or why it stopped said
static void job_check(struct app *app) {
	struct ops_job *job = app->job;
	if (!job) {
		return;
	}
	pthread_mutex_lock(&job->lock);
	bool finished = job->finished;
	char error[160], first[NAME_MAX + 1], dest[PATH_MAX];
	snprintf(error, sizeof(error), "%s", job->cancel ? "" : job->error);
	snprintf(first, sizeof(first), "%s", job->first);
	snprintf(dest, sizeof(dest), "%s", job->dest);
	pthread_mutex_unlock(&job->lock);
	if (!finished) {
		return;
	}
	ops_job_free(job);
	app->job = NULL;
	if (error[0]) {
		toast(app, "%s", error);
	}
	if (!strcmp(dest, app->folder.path)) {
		snprintf(app->select_next, sizeof(app->select_next), "%s", first);
		relist(app);
	}
}

static void menus(struct app *app) {
	struct fui *ui = app->ui;
	size_t sel = selected_count(app);
	bool trash = in_trash(app);
	if (fui_menu_is_open(ui, fui_id("items", 0))) {
		enum { OPEN, RENAME, TRASH, COPY, CUT, COPY_PATH, DUPLICATE, PUT_BACK, DELETE, PICK, COUNT };
		struct fui_menu_item items[12];
		int what[12], n = 0;
#define ITEM(id, label, key, off) \
	(items[n] = (struct fui_menu_item){label, key, off}, what[n++] = (id))
#define LINE() (items[n] = (struct fui_menu_item){0}, what[n++] = -1)
		if (app->pick) {
			ITEM(PICK, app->pick_folder ? "Choose" : "Open", "Enter", false);
		} else if (trash) {
			ITEM(PUT_BACK, "Put Back", NULL, false);
			ITEM(DELETE, "Delete Forever", "Shift+Del", false);
		} else {
			ITEM(OPEN, "Open", "Enter", false);
			LINE();
			ITEM(RENAME, "Rename", "F2", sel != 1);
			ITEM(DUPLICATE, "Duplicate", "Ctrl+D", false);
			ITEM(TRASH, "Move to Trash", "Del", false);
			LINE();
			ITEM(COPY, "Copy", "Ctrl+C", false);
			ITEM(CUT, "Cut", "Ctrl+X", false);
			ITEM(COPY_PATH, "Copy Path", "Ctrl+Shift+C", false);
		}
		int picked_item = fui_menu(ui, fui_id("items", 0), items, n);
		switch (picked_item >= 0 ? what[picked_item] : -1) {
		case OPEN:
		case PICK:
			for (size_t i = 0; i < app->shown_len; i++) {
				if (app->selected[app->shown[i]]) {
					open_file(app, i);
					break;
				}
			}
			break;
		case RENAME:
			start_rename(app);
			break;
		case DUPLICATE:
			duplicate(app);
			break;
		case TRASH:
			trash_selected(app);
			break;
		case COPY:
			clip_set(app, false);
			break;
		case CUT:
			clip_set(app, true);
			break;
		case COPY_PATH:
			copy_paths(app);
			break;
		case PUT_BACK:
			put_back(app);
			break;
		case DELETE:
			app->confirming = true;
			break;
		}
	}
	if (fui_menu_is_open(ui, fui_id("empty", 0))) {
		enum { NEW_FOLDER, PASTE, HIDDEN, ICONS, LIST, EMPTY };
		struct fui_menu_item items[10];
		int what[10], n = 0;
		if (!app->pick && !trash) {
			ITEM(NEW_FOLDER, "New Folder", "Ctrl+Shift+N", false);
			ITEM(PASTE, "Paste", "Ctrl+V", !fui_clipboard_has_files(ui));
			LINE();
		}
		if (trash && !app->pick) {
			ITEM(EMPTY, "Empty Trash", NULL, !app->folder.len);
			LINE();
		}
		ITEM(HIDDEN, app->hidden ? "Hide Hidden Files" : "Show Hidden Files", "Ctrl+H", false);
		ITEM(ICONS, "View as Icons", "Ctrl+1", !app->list_view);
		ITEM(LIST, "View as List", "Ctrl+2", app->list_view);
#undef ITEM
#undef LINE
		int picked_item = fui_menu(ui, fui_id("empty", 0), items, n);
		switch (picked_item >= 0 ? what[picked_item] : -1) {
		case NEW_FOLDER:
			new_folder(app);
			break;
		case PASTE:
			paste(app);
			break;
		case HIDDEN:
			app->hidden = !app->hidden;
			rebuild(app);
			break;
		case ICONS:
			app->list_view = false;
			break;
		case LIST:
			app->list_view = true;
			break;
		case EMPTY:
			selection_clear(app);
			for (size_t i = 0; i < app->shown_len; i++) {
				app->selected[app->shown[i]] = true;
			}
			app->confirming = true;
			break;
		}
	}
	(void)sel;
}

// asked before anything's deleted for good. drawn first into a layer of its
// own, so its buttons get the click before anything under it, shown last
static cairo_pattern_t *confirm(struct app *app) {
	struct fui *ui = app->ui;
	if (!app->confirming) {
		return NULL;
	}
	cairo_t *cr = fui_cairo(ui);
	cairo_push_group(cr);
	fui_fill_rounded(ui, 0, 0, W, H, 18, 0x00000066);
	const double w = 340, h = 150, x = (W - w) / 2, y = (H - h) / 2;
	fui_fill_rounded(ui, x, y, w, h, 14, 0x2b2b31ff);
	fui_rounded(ui, x + 0.5, y + 0.5, w - 1, h - 1, 14);
	fui_color(ui, 0xffffff24);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
	size_t n = selected_count(app);
	char question[96];
	snprintf(question, sizeof(question), "Delete %zu item%s forever?", n, n == 1 ? "" : "s");
	fui_text(ui, x + 22, y + 22, question, F_TITLE, TEXT, FUI_LEFT, w - 44);
	fui_paragraph(ui, x + 22, y + 48, w - 44, "This can't be undone.", F_TEXT, TEXT_DIM);
	double dw = fui_text_width(ui, "Delete", F_MEDIUM) + 28;
	double cw = fui_text_width(ui, "Cancel", F_MEDIUM) + 28;
	double by = y + h - 46;
	bool del = fui_button(ui, fui_id("confirm delete", 0), x + w - 22 - dw, by, "Delete")
		|| fui_key_pressed(ui, XKB_KEY_Return, 0);
	bool cancel = fui_button(ui, fui_id("confirm cancel", 0), x + w - 22 - dw - 10 - cw, by,
		"Cancel") || fui_key_pressed(ui, XKB_KEY_Escape, 0);
	// nothing under it takes a click
	fui_clicked(ui, 0, 0, W, H);
	fui_right_clicked(ui, 0, 0, W, H);
	cairo_pattern_t *layer = cairo_pop_group(cr);
	if (del) {
		app->confirming = false;
		delete_selected(app);
	} else if (cancel) {
		app->confirming = false;
	}
	return layer;
}

static void frame(struct fui *ui, void *data) {
	struct app *app = data;
	W = fui_width(ui);
	H = fui_height(ui);
	cairo_t *cr = fui_cairo(ui);
	app->icon = fui_ease(ui, fui_id("zoom icon", 0), app->icon_want, 20);
	app->row = fui_ease(ui, fui_id("zoom row", 0), app->row_want, 20);
	job_check(app);
	// the window: frosted glass, the desktop glowing through the sidebar,
	// the files on solid dark
	struct fui_pane pane = {0, 0, W, H, 14, 40, 0x1e1e2470, 1.9, 0.25, 0};
	fui_set_glass(ui, &pane, 1);
	if (!fui_has_glass(ui)) {
		fui_fill_rounded(ui, 0, 0, W, H, 14, 0x2a2a30ff);
	}
	const double r = 14;
	cairo_new_path(cr);
	cairo_move_to(cr, SIDEBAR_W, 0);
	cairo_arc(cr, W - r, r, r, -M_PI / 2, 0);
	cairo_arc(cr, W - r, H - r, r, 0, M_PI / 2);
	cairo_line_to(cr, SIDEBAR_W, H);
	cairo_close_path(cr);
	fui_color(ui, CONTENT_BG);
	cairo_fill(cr);
	fui_fill_rounded(ui, SIDEBAR_W, 0, 1, H, 0, 0x00000066);

	// the question answered this frame: its key isn't the window's too
	bool asking = app->confirming;
	cairo_pattern_t *question = confirm(app);
	if (!asking) {
		keys(app);
		typeahead(app);
	}
	dnd(app);
	pasted(app);
	sidebar(app);
	toolbar(app);
	view(app);
	drag_check(app);
	status_bar(app);
	menus(app);
	if (question) {
		cairo_set_source(cr, question);
		cairo_paint(cr);
		cairo_pattern_destroy(question);
	}
	// a refusal, for a few seconds
	double age = fui_time(ui) - app->toast_at;
	if (app->toast[0] && age < 4) {
		double fade = age < 3.5 ? 1 : (4 - age) / 0.5;
		double tw = fmin(fui_text_width(ui, app->toast, F_TEXT) + 28, VIEW_W - 40);
		double tx = VIEW_X + (VIEW_W - tw) / 2, ty = VIEW_BOTTOM - 46;
		fui_fill_rounded(ui, tx, ty, tw, 30, 15, (uint32_t)(0x3a1d1d00 | (uint32_t)(0xf0 * fade)));
		fui_text(ui, tx + 14, ty + 7, app->toast, F_TEXT,
			(uint32_t)(0xffffff00 | (uint32_t)(0xeb * fade)), FUI_LEFT, tw - 28);
		fui_redraw(ui); // keeps it fading
	}
	fui_rounded(ui, 0.5, 0.5, W - 1, H - 1, r);
	fui_color(ui, 0xffffff1c);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
}

static void changed_later(struct fui *ui, void *data) {
	relist(data);
	fui_redraw(ui);
}

static void folder_watch(struct fui *ui, int fd, void *data) {
	struct app *app = data;
	if (folder_changed(&app->folder)) {
		// a burst of changes (a big copy): once they settle a little
		fui_after(ui, 150, changed_later, app);
	}
}

static void usage(void) {
	fprintf(stderr, "usage: flamefiles [FOLDER] [--pick [--images] | --pick-folder]\n"
		"  --pick         choose a file, its path printed (exit 1: cancelled)\n"
		"  --images       only pictures can be chosen\n"
		"  --pick-folder  choose a folder\n");
}

int main(int argc, char **argv) {
	struct app app = {.status = 0, .icon = ICON_DEFAULT, .icon_want = ICON_DEFAULT,
		.row = ROW_DEFAULT, .row_want = ROW_DEFAULT, .drop_place = -1};
	const char *start = NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--pick")) {
			app.pick = true;
		} else if (!strcmp(argv[i], "--images")) {
			app.pick_images = true;
		} else if (!strcmp(argv[i], "--pick-folder")) {
			app.pick = app.pick_folder = true;
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage();
			return 0;
		} else if (argv[i][0] == '-') {
			usage();
			return 2;
		} else {
			start = argv[i];
		}
	}
	if (app.pick) {
		app.status = 1; // closed without a pick: cancelled
	}
	folder_init(&app.folder);
	places_load(&app);
	prefs_load(&app);
	app.ui = fui_create(&(struct fui_config){
		// a chooser floats over what asked for it (flame's rule for it)
		.app_id = app.pick ? "flamefiles-chooser" : "flamefiles",
		.title = app.pick ? "Choose" : "Files",
		.width = app.pick ? 860 : 1000,
		.height = app.pick ? 560 : 640,
		.min_width = MIN_W,
		.min_height = MIN_H,
		.corner_radius = 14,
		.frame = frame,
		.data = &app,
	});
	if (!app.ui) {
		fprintf(stderr, "flamefiles: no wayland display\n");
		return 1;
	}
	char where[PATH_MAX];
	if (start && realpath(start, where)) {
		struct stat st;
		if (stat(where, &st) == 0 && !S_ISDIR(st.st_mode)) {
			// a file: its folder, it selected
			char *slash = strrchr(where, '/');
			if (slash) {
				snprintf(app.select_next, sizeof(app.select_next), "%s", slash + 1);
				*slash = '\0';
				if (!where[0]) {
					snprintf(where, sizeof(where), "/");
				}
			}
		}
	} else {
		snprintf(where, sizeof(where), "%s", home_dir());
	}
	if (!go(&app, where, false) && !go(&app, home_dir(), false)) {
		go(&app, "/", false);
	}
	if (app.select_next[0]) {
		relist(&app);
	}
	if (app.folder.inotify >= 0) {
		fui_watch(app.ui, app.folder.inotify, folder_watch, &app);
	}
	int status = fui_run(app.ui);
	prefs_save(&app);
	if (app.job) {
		ops_job_free(app.job); // stopped where it is
	}
	fui_destroy(app.ui);
	folder_finish(&app.folder);
	free(app.shown);
	free(app.selected);
	free_paths(app.back, app.back_len);
	free_paths(app.forward, app.forward_len);
	return status ? status : app.status;
}
