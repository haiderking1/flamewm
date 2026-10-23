#include "apps.h"

#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "match.h"
#include "usage.h"

struct app {
	GAppInfo *info;
	char *name, *icon, *id;
	// what it's found by, folded: its name, what kind of app it is and its
	// keywords ("browser"), the command it runs
	char *folded, *about, *command;
};

static struct app *apps;
static int len;
static bool stale = true;

static void changed(GAppInfoMonitor *monitor, gpointer data) {
	stale = true;
}

static char *folded(const char *text) {
	char out[1024];
	fold(text ? text : "", out, sizeof(out));
	return strdup(out);
}

static void clear(void) {
	for (int i = 0; i < len; i++) {
		g_object_unref(apps[i].info);
		free(apps[i].name);
		free(apps[i].icon);
		free(apps[i].id);
		free(apps[i].folded);
		free(apps[i].about);
		free(apps[i].command);
	}
	free(apps);
	apps = NULL;
	len = 0;
}

static void read_all(void) {
	clear();
	GList *all = g_app_info_get_all();
	int n = (int)g_list_length(all);
	apps = calloc((size_t)(n ? n : 1), sizeof(*apps));
	for (GList *l = all; l && apps; l = l->next) {
		GAppInfo *info = l->data;
		if (!g_app_info_should_show(info) || !G_IS_DESKTOP_APP_INFO(info)) {
			continue;
		}
		GDesktopAppInfo *desktop = G_DESKTOP_APP_INFO(info);
		struct app *a = &apps[len++];
		a->info = g_object_ref(info);
		a->name = strdup(g_app_info_get_name(info));
		char *icon = g_desktop_app_info_get_string(desktop, "Icon");
		a->icon = strdup(icon ? icon : "");
		g_free(icon);
		a->id = strdup(g_app_info_get_id(info) ? g_app_info_get_id(info) : a->name);
		a->folded = folded(a->name);
		GString *about = g_string_new(g_desktop_app_info_get_generic_name(desktop));
		const char *const *keywords = g_desktop_app_info_get_keywords(desktop);
		for (int k = 0; keywords && keywords[k]; k++) {
			g_string_append_printf(about, " %s", keywords[k]);
		}
		a->about = folded(about->str);
		g_string_free(about, TRUE);
		const char *exe = g_app_info_get_executable(info);
		const char *base = exe ? strrchr(exe, '/') : NULL;
		a->command = folded(base ? base + 1 : exe);
	}
	g_list_free_full(all, g_object_unref);
	stale = false;
}

void apps_load(void) {
	g_signal_connect(g_app_info_monitor_get(), "changed", G_CALLBACK(changed), NULL);
	read_all();
}

void apps_refresh(void) {
	// the monitor hears of new .desktop files through glib's loop: what it
	// has to say, said
	while (g_main_context_iteration(NULL, FALSE)) {
	}
	if (stale) {
		read_all();
	}
}

int apps_search(const char *query, struct app_hit *out, int max) {
	int found = 0;
	for (int i = 0; i < len; i++) {
		const struct app *a = &apps[i];
		int score = match(query, a->folded, true);
		// what kind it is, its keywords: below its own name
		int about = match(query, a->about, false);
		score = score > about - 250 ? score : about - 250;
		int command = match(query, a->command, false);
		score = score > command - 200 ? score : command - 200;
		if (score <= 0) {
			continue;
		}
		score += usage_boost(a->id);
		// in its place among the best so far
		int at = found < max ? found : max - 1;
		if (found == max && out[at].score >= score) {
			continue;
		}
		while (at > 0 && out[at - 1].score < score) {
			out[at] = out[at - 1];
			at--;
		}
		out[at] = (struct app_hit){i, score};
		found += found < max;
	}
	return found;
}

const char *apps_name(int i) {
	return apps[i].name;
}

const char *apps_icon(int i) {
	return apps[i].icon;
}

const char *apps_id(int i) {
	return apps[i].id;
}

bool apps_open(int i) {
	GError *error = NULL;
	GAppLaunchContext *context = g_app_launch_context_new();
	// started on its own (not this one's child to wait for)
	bool ok = g_desktop_app_info_launch_uris_as_manager(G_DESKTOP_APP_INFO(apps[i].info), NULL,
		context, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &error);
	if (!ok) {
		fprintf(stderr, "flamesearch: %s won't open: %s\n", apps[i].name,
			error ? error->message : "?");
		g_clear_error(&error);
	}
	g_object_unref(context);
	usage_add(apps[i].id);
	return ok;
}
