// flamesearch: one field over everything to find what's wanted as it's
// typed, apps, files in home, a sum's answer, and open it. it stays running
// once it's started, hidden: run again, it's up (or down) at once, the run
// only tells the one there is. super+d

#include <errno.h>
#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "../flameui/flameui.h"
#include "apps.h"
#include "calc.h"
#include "files.h"
#include "match.h"
#include "usage.h"

#define W 680
#define FIELD_H 56
#define ROW_H 40
#define HEAD_H 26
#define RADIUS 26
#define APPS_SHOWN 5
#define FILES_SHOWN 5
#define RESULTS (1 + APPS_SHOWN + FILES_SHOWN + 1)

#define TEXT 0xffffffff
#define TEXT_DIM 0xffffff99
#define PICKED 0x0a84ffe6 // the row enter opens
#define F_FIELD "Inter 17"
#define QUERY 1 // the query's line, its caret
#define F_ROW "Inter Medium 11"
#define F_SUB "Inter 10"
#define F_HEAD "Inter Semi-Bold 9"

enum kind { CALC, APP, FILE_, WEB };

struct result {
	enum kind kind;
	char title[256], sub[PATH_MAX];
	char icon[PATH_MAX]; // a name or a file
	bool thumb; // the icon's a picture to show small (a photo, a video)
	int app; // apps' index
	char path[PATH_MAX]; // a file's
	bool dir;
	bool head; // first of its kind: its kind's name over it
};

struct search {
	struct fui *ui;
	int listen; // the socket the next runs tell this one through
	bool open;
	char query[512];
	char searched[512]; // what the results are for
	struct result results[RESULTS];
	int len, picked;
	double typed_at; // the caret's lit a while after a key
	char browser_icon[PATH_MAX]; // the web's row
	double mx, my; // where the pointer was: moved, the row under it's picked
};

// ---- finding ----

static void home_short(const char *path, char *out, size_t size) {
	const char *home = getenv("HOME");
	size_t n = home ? strlen(home) : 0;
	if (n && !strncmp(path, home, n) && (path[n] == '/' || !path[n])) {
		snprintf(out, size, "~%s", path + n);
	} else {
		snprintf(out, size, "%s", path);
	}
}

static bool picture_name(const char *name) {
	static const char *const kinds[] = {
		".jpg", ".jpeg", ".png", ".webp", ".mp4", ".mkv", ".webm", ".mov", ".avi",
	};
	size_t len = strlen(name);
	for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
		size_t k = strlen(kinds[i]);
		if (len > k && !strcasecmp(name + len - k, kinds[i])) {
			return true;
		}
	}
	return false;
}

// a file's icon: a picture's itself, else its kind's from the icon theme
static void file_icon(struct result *r) {
	if (r->dir) {
		snprintf(r->icon, sizeof(r->icon), "folder");
		return;
	}
	if (picture_name(r->path)) {
		snprintf(r->icon, sizeof(r->icon), "%s", r->path);
		r->thumb = true;
		return;
	}
	char *type = g_content_type_guess(r->path, NULL, 0, NULL);
	char *icon = type ? g_content_type_get_generic_icon_name(type) : NULL;
	snprintf(r->icon, sizeof(r->icon), "%s", icon ? icon : "text-x-generic");
	g_free(icon);
	g_free(type);
}

// the query as typed, its spaces at the ends left out
static void add_web(struct search *s, const char *query) {
	struct result *r = &s->results[s->len++];
	*r = (struct result){.kind = WEB, .head = true};
	snprintf(r->title, sizeof(r->title), "Search the web for “%s”", query);
	snprintf(r->sub, sizeof(r->sub), "Google");
	snprintf(r->icon, sizeof(r->icon), "%s", s->browser_icon);
}

static void search(struct search *s) {
	snprintf(s->searched, sizeof(s->searched), "%s", s->query);
	s->len = 0;
	s->picked = 0;
	char query[512];
	const char *start = s->query;
	while (*start == ' ') {
		start++;
	}
	fold(start, query, sizeof(query));
	size_t qlen = strlen(query);
	while (qlen && query[qlen - 1] == ' ') {
		query[--qlen] = '\0';
	}
	if (!qlen) {
		return;
	}
	char answer[64];
	if (calc(start, answer, sizeof(answer))) {
		struct result *r = &s->results[s->len++];
		*r = (struct result){.kind = CALC, .head = true};
		snprintf(r->title, sizeof(r->title), "= %s", answer);
		snprintf(r->sub, sizeof(r->sub), "Enter to copy");
		snprintf(r->path, sizeof(r->path), "%s", answer);
		snprintf(r->icon, sizeof(r->icon), "accessories-calculator");
	}
	struct app_hit apps[APPS_SHOWN];
	int n_apps = apps_search(query, apps, APPS_SHOWN);
	static struct file_hit files[FILES_SHOWN];
	int n_files = qlen >= 2 ? files_search(query, files, FILES_SHOWN) : 0;
	// the kind with the best match goes first: that's what's looked for.
	// an app's the likelier: a file has to match clearly better
	bool files_first = n_files && (!n_apps || files[0].score > apps[0].score + 200);
	for (int pass = 0; pass < 2; pass++) {
		if ((pass == 0) == !files_first) {
			for (int i = 0; i < n_apps; i++) {
				struct result *r = &s->results[s->len++];
				*r = (struct result){.kind = APP, .app = apps[i].index, .head = i == 0};
				snprintf(r->title, sizeof(r->title), "%s", apps_name(apps[i].index));
				snprintf(r->sub, sizeof(r->sub), "Application");
				snprintf(r->icon, sizeof(r->icon), "%s", apps_icon(apps[i].index));
			}
		} else {
			for (int i = 0; i < n_files; i++) {
				struct result *r = &s->results[s->len++];
				*r = (struct result){.kind = FILE_, .dir = files[i].dir, .head = i == 0};
				snprintf(r->path, sizeof(r->path), "%s", files[i].path);
				const char *slash = strrchr(files[i].path, '/');
				snprintf(r->title, sizeof(r->title), "%s", slash ? slash + 1 : files[i].path);
				char parent[PATH_MAX];
				snprintf(parent, sizeof(parent), "%s", files[i].path);
				char *cut = strrchr(parent, '/');
				if (cut) {
					*cut = '\0';
				}
				home_short(parent, r->sub, sizeof(r->sub));
				file_icon(r);
			}
		}
	}
	char typed[512];
	snprintf(typed, sizeof(typed), "%s", start);
	for (size_t n = strlen(typed); n && typed[n - 1] == ' '; n--) {
		typed[n - 1] = '\0';
	}
	add_web(s, typed);
}

static const char *kind_name(enum kind kind) {
	return kind == CALC ? "Calculator" : kind == APP ? "Applications"
		: kind == FILE_ ? "Files" : "Web";
}

// ---- opening ----

// flame's app called name: the one next to this one first (a build dir,
// or wherever flame's installed), else whichever's on the PATH
static void own_app(const char *name, char *out, size_t size) {
	char self[PATH_MAX];
	ssize_t len = readlink("/proc/self/exe", self, sizeof(self) - 1);
	if (len > 0) {
		self[len] = '\0';
		char *slash = strrchr(self, '/');
		if (slash) {
			*slash = '\0';
			snprintf(out, size, "%s/%s", self, name);
			if (access(out, X_OK) == 0) {
				return;
			}
		}
	}
	snprintf(out, size, "%s", name);
}

// a folder in flamefiles. no flamefiles: in what opens folders here
static void spawn_files(const char *folder) {
	char files[PATH_MAX];
	own_app("flamefiles", files, sizeof(files));
	char *argv[] = {files, (char *)folder, NULL};
	GError *error = NULL;
	// on its own, not this one's child to wait for
	if (g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &error)) {
		return;
	}
	fprintf(stderr, "flamesearch: no flamefiles (%s), %s in what opens folders\n",
		error->message, folder);
	g_clear_error(&error);
	char *uri = g_filename_to_uri(folder, NULL, NULL);
	if (!uri || !g_app_info_launch_default_for_uri(uri, NULL, &error)) {
		fprintf(stderr, "flamesearch: nothing opens %s: %s\n", folder,
			error ? error->message : "?");
		g_clear_error(&error);
	}
	g_free(uri);
}

// a file in the app that opens its kind, a folder in flamefiles. nothing
// opens it: its folder, with it there
static void open_file(const char *path, bool dir) {
	usage_add(path);
	if (dir) {
		spawn_files(path);
		return;
	}
	char *type = g_content_type_guess(path, NULL, 0, NULL);
	GAppInfo *app = type ? g_app_info_get_default_for_type(type, FALSE) : NULL;
	g_free(type);
	bool opened = false;
	if (app) {
		char *uri = g_filename_to_uri(path, NULL, NULL);
		GList uris = {.data = uri};
		GAppLaunchContext *context = g_app_launch_context_new();
		opened = G_IS_DESKTOP_APP_INFO(app)
			? g_desktop_app_info_launch_uris_as_manager(G_DESKTOP_APP_INFO(app), &uris, context,
				G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL)
			: g_app_info_launch_uris(app, &uris, context, NULL);
		g_object_unref(context);
		g_free(uri);
		g_object_unref(app);
	}
	if (!opened) {
		char folder[PATH_MAX];
		snprintf(folder, sizeof(folder), "%s", path);
		char *slash = strrchr(folder, '/');
		if (slash) {
			*slash = '\0';
		}
		spawn_files(folder);
	}
}

static void close_search(struct search *s) {
	s->open = false;
	fui_redraw(s->ui);
}

// the row picked, opened. reveal: a file's folder instead
static void open_result(struct search *s, int i, bool reveal) {
	if (i < 0 || i >= s->len) {
		return;
	}
	struct result *r = &s->results[i];
	switch (r->kind) {
	case CALC:
		fui_copy_text(s->ui, r->path);
		break;
	case APP:
		apps_open(r->app);
		break;
	case FILE_:
		if (reveal) {
			char folder[PATH_MAX];
			snprintf(folder, sizeof(folder), "%s", r->path);
			char *slash = strrchr(folder, '/');
			if (slash) {
				*slash = '\0';
			}
			spawn_files(folder);
		} else {
			open_file(r->path, r->dir);
		}
		break;
	case WEB: {
		char *q = g_uri_escape_string(s->query, NULL, FALSE);
		char url[2048];
		snprintf(url, sizeof(url), "https://www.google.com/search?q=%s", q);
		g_free(q);
		g_app_info_launch_default_for_uri(url, NULL, NULL);
		break;
	}
	}
	close_search(s);
}

// ---- the keys ----

static void keys(struct fui *ui, struct search *s) {
	// one by one as they went down: a b backspace c, fast, is "ac"
	uint32_t sym;
	unsigned mods;
	const char *text;
	for (int i = 0; fui_key_at(ui, i, &sym, &mods, &text); i++) {
		bool plain = !(mods & (FUI_CTRL | FUI_ALT | FUI_SUPER));
		bool ctrl = mods == FUI_CTRL;
		bool edited = true;
		if (ctrl && (sym == XKB_KEY_u || sym == XKB_KEY_U)) {
			s->query[0] = '\0';
		} else if (sym == XKB_KEY_Escape) {
			if (*s->query) {
				s->query[0] = '\0';
			} else {
				close_search(s);
				return;
			}
		} else if (!fui_edit_key(ui, QUERY, i, s->query, sizeof(s->query))) {
			edited = false;
		}
		if (edited) {
			s->typed_at = fui_time(ui);
		}
		// what's found follows what's typed before any key that picks
		if (strcmp(s->query, s->searched)) {
			search(s);
		}
		if (sym == XKB_KEY_Down || (sym == XKB_KEY_Tab && plain)
				|| (ctrl && (sym == XKB_KEY_n || sym == XKB_KEY_N))) {
			s->picked = s->picked + 1 < s->len ? s->picked + 1 : s->picked;
		} else if (sym == XKB_KEY_Up || sym == XKB_KEY_ISO_Left_Tab
				|| (ctrl && (sym == XKB_KEY_p || sym == XKB_KEY_P))) {
			s->picked = s->picked > 0 ? s->picked - 1 : 0;
		} else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
			// ctrl: a file's folder instead
			open_result(s, s->picked, ctrl);
			return;
		}
	}
	if (strcmp(s->query, s->searched)) {
		search(s);
	}
}

// ---- drawing ----

static void magnifier(cairo_t *cr, double cx, double cy, double s) {
	cairo_new_path(cr);
	cairo_arc(cr, cx - s * 0.08, cy - s * 0.08, s * 0.3, 0, 2 * M_PI);
	cairo_move_to(cr, cx + s * 0.14, cy + s * 0.14);
	cairo_line_to(cr, cx + s * 0.4, cy + s * 0.4);
	cairo_set_line_width(cr, s * 0.1);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_stroke(cr);
}

// a picture whole in an s box at x, y
static void picture_at(cairo_t *cr, cairo_surface_t *picture, double x, double y, double s,
		double r) {
	int pw = cairo_image_surface_get_width(picture), ph = cairo_image_surface_get_height(picture);
	if (pw <= 0 || ph <= 0) {
		return;
	}
	double k = fmin(s / pw, s / ph);
	cairo_save(cr);
	cairo_translate(cr, x + (s - pw * k) / 2, y + (s - ph * k) / 2);
	cairo_scale(cr, k, k);
	if (r > 0) {
		cairo_new_path(cr);
		double w = pw, h = ph, rr = r / k;
		cairo_arc(cr, w - rr, rr, rr, -M_PI / 2, 0);
		cairo_arc(cr, w - rr, h - rr, rr, 0, M_PI / 2);
		cairo_arc(cr, rr, h - rr, rr, M_PI / 2, M_PI);
		cairo_arc(cr, rr, rr, rr, M_PI, 3 * M_PI / 2);
		cairo_close_path(cr);
		cairo_clip(cr);
	}
	cairo_set_source_surface(cr, picture, 0, 0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
	cairo_paint(cr);
	cairo_restore(cr);
}

static void row_icon(struct fui *ui, const struct result *r, double x, double y, double s) {
	cairo_t *cr = fui_cairo(ui);
	bool failed = false;
	cairo_surface_t *icon = r->thumb ? fui_thumb_fit(ui, r->icon, (int)s * 2, &failed)
		: fui_icon(ui, r->icon, (int)s * 2, &failed);
	if (icon) {
		picture_at(cr, icon, x, y, s, r->thumb ? 4 : 0);
		return;
	}
	if (!failed) {
		return; // on its way
	}
	// none: a plain one of its kind
	fui_fill_rounded(ui, x + s * 0.1, y + s * 0.1, s * 0.8, s * 0.8, s * 0.2,
		r->kind == CALC ? 0xff9f0aff : r->kind == APP ? 0x636366ff : 0x8e8e93ff);
	fui_color(ui, TEXT);
	if (r->kind == CALC) {
		fui_text(ui, x + s / 2, y + s * 0.2, "=", "Inter Semi-Bold 13", TEXT, FUI_CENTER, 0);
	} else if (r->kind == WEB) {
		magnifier(cr, x + s / 2, y + s / 2, s * 0.6);
	}
}

static double results_height(const struct search *s) {
	double h = 0;
	for (int i = 0; i < s->len; i++) {
		h += ROW_H + (s->results[i].head ? HEAD_H : 0);
	}
	return s->len ? h + 10 : 0;
}

static void caret_blink(struct fui *ui, void *data) {
	struct search *s = data;
	if (s->open) {
		fui_redraw(ui);
	}
}

static void frame(struct fui *ui, void *data) {
	struct search *s = data;
	cairo_t *cr = fui_cairo(ui);
	if (s->open) {
		keys(ui, s);
	}
	// in from a touch smaller, out the same way: then it's down
	double shown = fui_glide_from(ui, fui_id("shown", 0), 0, s->open ? 1 : 0, s->open ? 0.2 : 0.14);
	if (!s->open && shown <= 0.001) {
		fui_set_glass(ui, NULL, 0);
		fui_panel_show(ui, false);
		return;
	}
	double sw = fui_width(ui), sh = fui_height(ui);
	double h = fui_glide(ui, fui_id("height", 0), FIELD_H + (s->len ? 1 + results_height(s) : 0),
		0.22);
	double x = round((sw - W) / 2), y = round(sh * 0.22);
	double k = 0.96 + 0.04 * shown; // how big, around its top's middle
	double gx = x + W * (1 - k) / 2, gw = W * k, gh = h * k;
	fui_set_glass(ui, &(struct fui_pane){(int)gx, (int)y, (int)gw, (int)gh, RADIUS * k, 18,
		0x00000059, 1.6, 0.15, 22, 0}, 1);
	fui_set_glass_opacity(ui, shown);

	cairo_save(cr);
	cairo_push_group(cr);
	cairo_translate(cr, x + W / 2.0, y);
	cairo_scale(cr, k, k);
	cairo_translate(cr, -W / 2.0, 0);
	fui_rounded(ui, 0, 0, W, h, RADIUS);
	cairo_clip(cr);

	// the field
	fui_color(ui, TEXT_DIM);
	magnifier(cr, 28, FIELD_H / 2.0, 22);
	double tx = 52;
	if (!*s->query) {
		fui_text(ui, tx + 4, FIELD_H / 2.0 - 12, "Search", F_FIELD, TEXT_DIM, FUI_LEFT, 0);
	}
	// the caret: lit while typing, blinking after
	double now = fui_time(ui);
	bool caret_on = now - s->typed_at < 0.6 || fmod(now - s->typed_at, 1.0) < 0.5;
	if (fui_edit_line(ui, QUERY, tx, 0, W - tx - 24, FIELD_H, s->query, sizeof(s->query),
			F_FIELD, TEXT, 0x0a84ffff, caret_on || !*s->query)) {
		s->typed_at = now;
	}
	fui_after(ui, 500, caret_blink, s);

	// what's found, under a line
	if (s->len) {
		fui_fill_rounded(ui, 16, FIELD_H, W - 32, 1, 0, 0xffffff1f);
	}
	double px, py;
	fui_pointer(ui, &px, &py);
	bool moved = px != s->mx || py != s->my;
	s->mx = px;
	s->my = py;
	double ry = FIELD_H + 6;
	for (int i = 0; i < s->len; i++) {
		struct result *r = &s->results[i];
		if (r->head) {
			fui_text(ui, 22, ry + 9, kind_name(r->kind), F_HEAD, TEXT_DIM, FUI_LEFT, 0);
			ry += HEAD_H;
		}
		// pointed at it's picked, clicked it's opened
		double rx = 8, rw = W - 16;
		bool over = fui_hover(ui, x + rx, y + ry, rw, ROW_H);
		if (over && moved) {
			s->picked = i;
		}
		double lit = fui_glide(ui, fui_id("picked", (uint64_t)i), i == s->picked ? 1 : 0, 0.12);
		if (lit > 0.01) {
			fui_fill_rounded(ui, rx, ry, rw, ROW_H, 10,
				(PICKED & 0xffffff00) | (uint32_t)((PICKED & 0xff) * lit));
		}
		row_icon(ui, r, rx + 10, ry + 6, 28);
		double sub_w = fui_text_width(ui, r->sub, F_SUB);
		sub_w = fmin(sub_w, rw * 0.45);
		fui_text(ui, rx + rw - 14, ry + 12, r->sub, F_SUB, lit > 0.5 ? 0xffffffcc : TEXT_DIM,
			FUI_RIGHT, sub_w);
		fui_text(ui, rx + 48, ry + 11, r->title, F_ROW, TEXT, FUI_LEFT,
			rw - 48 - sub_w - 30);
		if (over) {
			fui_cursor_pointer(ui);
		}
		if (fui_clicked(ui, x + rx, y + ry, rw, ROW_H)) {
			open_result(s, i, false);
		}
		ry += ROW_H;
	}
	cairo_pop_group_to_source(cr);
	cairo_paint_with_alpha(cr, shown); // before the restore: that'd put back the source too
	cairo_restore(cr);

	// a click anywhere else: gone
	if (s->open && fui_clicked(ui, 0, 0, sw, sh) && !fui_hover(ui, x, y, W, h)) {
		close_search(s);
	}
}

// ---- one running ----

static void socket_path(char *out, size_t size) {
	const char *dir = getenv("XDG_RUNTIME_DIR"), *display = getenv("WAYLAND_DISPLAY");
	snprintf(out, size, "%s/flamesearch-%s.sock", dir ? dir : "/tmp",
		display ? display : "wayland-0");
}

// the one running told to go up or down: false when there's none
static bool tell_running(const char *path) {
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	bool told = fd >= 0 && connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0
		&& write(fd, "toggle\n", 7) == 7;
	if (fd >= 0) {
		close(fd);
	}
	return told;
}

static int listen_on(const char *path) {
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	unlink(path); // one that's left from a run that's gone (nobody answered on it)
	if (fd < 0 || bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 4) < 0) {
		if (fd >= 0) {
			close(fd);
		}
		return -1;
	}
	return fd;
}

static void show(struct search *s) {
	s->open = true;
	s->query[0] = s->searched[0] = '\0';
	s->len = 0;
	apps_refresh();
	files_refresh();
	fui_panel_show(s->ui, true);
	fui_redraw(s->ui);
}

static void told(struct fui *ui, int fd, void *data) {
	struct search *s = data;
	int client = accept(fd, NULL, NULL);
	if (client < 0) {
		return;
	}
	char line[32] = {0};
	if (read(client, line, sizeof(line) - 1) > 0 && !strncmp(line, "toggle", 6)) {
		if (s->open) {
			close_search(s);
		} else {
			show(s);
		}
	}
	close(client);
}

int main(int argc, char **argv) {
	char path[256];
	socket_path(path, sizeof(path));
	if (tell_running(path)) {
		return 0;
	}
	static struct search s;
	s.listen = listen_on(path);
	if (s.listen < 0) {
		// another one started a moment ago: tell that one
		return tell_running(path) ? 0 : 1;
	}
	// the browser's icon for the web's row
	GAppInfo *browser = g_app_info_get_default_for_uri_scheme("https");
	if (browser && G_IS_DESKTOP_APP_INFO(browser)) {
		char *icon = g_desktop_app_info_get_string(G_DESKTOP_APP_INFO(browser), "Icon");
		snprintf(s.browser_icon, sizeof(s.browser_icon), "%s", icon ? icon : "");
		g_free(icon);
	}
	if (browser) {
		g_object_unref(browser);
	}
	usage_load();
	apps_load();
	files_start();
	s.ui = fui_create(&(struct fui_config){
		.app_id = "flamesearch",
		.title = "Search",
		.panel = &(struct fui_panel){
			.edges = FUI_EDGE_TOP | FUI_EDGE_BOTTOM | FUI_EDGE_LEFT | FUI_EDGE_RIGHT,
			.room = -1,
			.over_fullscreen = true,
			.grab_keys = true,
			.hidden = true,
		},
		.frame = frame,
		.data = &s,
	});
	if (!s.ui) {
		fprintf(stderr, "flamesearch: no wayland display\n");
		unlink(path);
		return 1;
	}
	fui_watch(s.ui, s.listen, told, &s);
	show(&s);
	int status = fui_run(s.ui);
	fui_destroy(s.ui);
	files_stop();
	close(s.listen);
	unlink(path);
	return status;
}
