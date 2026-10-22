#include "config/settings.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wlr/types/wlr_output.h>

#include "config/config.h"
#include "core/server.h"
#include "image/image.h"
#include "util/strbuf.h"

// one key a line in settings.lua, its key after it as a comment so the next
// change finds it: flame.set{ gap = 8 } -- gap. lines without one (written
// by hand) stay as they are

enum kind {
	KIND_INT,
	KIND_FLOAT,
	KIND_BOOL,
	KIND_COLOR,
	KIND_STRING,
	KIND_ENUM,
};

static const char *const x_keys_names[] = {"never", "modifiers", "combos", "always", NULL};
static const char *const vrr_names[] = {"off", "fullscreen", "always", NULL};
static const char *const pace_names[] = {"off", "vrr", "always", NULL};
static const char *const wallpaper_modes[] = {"fill", "fit", "center", "stretch", NULL};

struct setting {
	const char *key;
	enum kind kind;
	double min, max;
	const char *const *names;
};

static const struct setting settings[] = {
	{"gap", KIND_INT, 0, 200, NULL},
	{"border_width", KIND_INT, 0, 50, NULL},
	{"border_color", KIND_COLOR, 0, 0, NULL},
	{"border_focused_color", KIND_COLOR, 0, 0, NULL},
	{"background", KIND_COLOR, 0, 0, NULL},
	{"wallpaper", KIND_STRING, 0, 0, NULL},
	{"wallpaper_mode", KIND_ENUM, 0, 0, wallpaper_modes},
	{"focus_follows_mouse", KIND_BOOL, 0, 0, NULL},
	{"repeat_rate", KIND_INT, 1, 200, NULL},
	{"repeat_delay", KIND_INT, 50, 2000, NULL},
	{"cursor_theme", KIND_STRING, 0, 0, NULL},
	{"cursor_size", KIND_INT, 8, 256, NULL},
	{"keyboard.layout", KIND_STRING, 0, 0, NULL},
	{"keyboard.variant", KIND_STRING, 0, 0, NULL},
	{"keyboard.options", KIND_STRING, 0, 0, NULL},
	{"keyboard.model", KIND_STRING, 0, 0, NULL},
	{"animations.enabled", KIND_BOOL, 0, 0, NULL},
	{"animations.unfocused_opacity", KIND_FLOAT, 0.1, 1, NULL},
	{"allow_tearing", KIND_BOOL, 0, 0, NULL},
	{"vrr", KIND_ENUM, 0, 0, vrr_names},
	{"pace", KIND_ENUM, 0, 0, pace_names},
	{"pace_fps", KIND_INT, 0, 1000, NULL},
	{"x_zero_scaling", KIND_BOOL, 0, 0, NULL},
	{"x_global_keys", KIND_ENUM, 0, 0, x_keys_names},
};

static const struct setting output_scale = {"output.*.scale", KIND_FLOAT, 0.25, 8, NULL};

// the setting for key, output scales by any screen's name
static const struct setting *find(const char *key, char *output, size_t size) {
	for (size_t i = 0; i < sizeof(settings) / sizeof(settings[0]); i++) {
		if (!strcmp(settings[i].key, key)) {
			return &settings[i];
		}
	}
	size_t len = strlen(key);
	if (!strncmp(key, "output.", 7) && len > 13 && !strcmp(key + len - 6, ".scale")
			&& len - 13 < size) {
		memcpy(output, key + 7, len - 13);
		output[len - 13] = '\0';
		return !strchr(output, '"') && !strchr(output, '\\') ? &output_scale : NULL;
	}
	return NULL;
}

static bool pinned(const struct flame_config *config, const char *key) {
	for (size_t i = 0; i < config->pinned_count; i++) {
		if (!strcmp(config->pinned[i], key)) {
			return true;
		}
	}
	return false;
}

// ---- values out ----

static void write_color(struct strbuf *out, const float c[4]) {
	// kept premultiplied, shown straight
	float a = c[3] > 0 ? c[3] : 1;
	unsigned r = (unsigned)lroundf(fminf(c[0] / a, 1) * 255);
	unsigned g = (unsigned)lroundf(fminf(c[1] / a, 1) * 255);
	unsigned b = (unsigned)lroundf(fminf(c[2] / a, 1) * 255);
	unsigned alpha = (unsigned)lroundf(c[3] * 255);
	if (alpha == 255) {
		strbuf_printf(out, "\"#%02x%02x%02x\"", r, g, b);
	} else {
		strbuf_printf(out, "\"#%02x%02x%02x%02x\"", r, g, b, alpha);
	}
}

static void write_value(struct strbuf *out, const struct flame_config *c, const char *key) {
	const struct flame_keyboard_names *k = &c->keyboard;
	if (!strcmp(key, "gap")) {
		strbuf_printf(out, "%d", c->gap);
	} else if (!strcmp(key, "border_width")) {
		strbuf_printf(out, "%d", c->border_width);
	} else if (!strcmp(key, "border_color")) {
		write_color(out, c->border_color);
	} else if (!strcmp(key, "border_focused_color")) {
		write_color(out, c->border_focused_color);
	} else if (!strcmp(key, "background")) {
		write_color(out, c->background_color);
	} else if (!strcmp(key, "wallpaper")) {
		strbuf_json_string(out, c->wallpaper ? c->wallpaper : "");
	} else if (!strcmp(key, "wallpaper_mode")) {
		strbuf_json_string(out, wallpaper_modes[c->wallpaper_mode]);
	} else if (!strcmp(key, "focus_follows_mouse")) {
		strbuf_printf(out, "%s", c->focus_follows_mouse ? "true" : "false");
	} else if (!strcmp(key, "repeat_rate")) {
		strbuf_printf(out, "%d", c->repeat_rate);
	} else if (!strcmp(key, "repeat_delay")) {
		strbuf_printf(out, "%d", c->repeat_delay);
	} else if (!strcmp(key, "cursor_theme")) {
		strbuf_json_string(out, c->cursor_theme ? c->cursor_theme : "");
	} else if (!strcmp(key, "cursor_size")) {
		strbuf_printf(out, "%u", c->cursor_size);
	} else if (!strcmp(key, "keyboard.layout")) {
		strbuf_json_string(out, k->layout ? k->layout : "");
	} else if (!strcmp(key, "keyboard.variant")) {
		strbuf_json_string(out, k->variant ? k->variant : "");
	} else if (!strcmp(key, "keyboard.options")) {
		strbuf_json_string(out, k->options ? k->options : "");
	} else if (!strcmp(key, "keyboard.model")) {
		strbuf_json_string(out, k->model ? k->model : "");
	} else if (!strcmp(key, "animations.enabled")) {
		strbuf_printf(out, "%s", c->anim.enabled ? "true" : "false");
	} else if (!strcmp(key, "animations.unfocused_opacity")) {
		strbuf_printf(out, "%.2f", c->anim.unfocused_opacity);
	} else if (!strcmp(key, "allow_tearing")) {
		strbuf_printf(out, "%s", c->allow_tearing ? "true" : "false");
	} else if (!strcmp(key, "vrr")) {
		strbuf_json_string(out, vrr_names[c->vrr]);
	} else if (!strcmp(key, "pace")) {
		strbuf_json_string(out, pace_names[c->pace]);
	} else if (!strcmp(key, "pace_fps")) {
		strbuf_printf(out, "%d", c->pace_fps);
	} else if (!strcmp(key, "x_zero_scaling")) {
		strbuf_printf(out, "%s", c->x_zero_scaling ? "true" : "false");
	} else if (!strcmp(key, "x_global_keys")) {
		strbuf_json_string(out, x_keys_names[c->x_global_keys]);
	} else {
		strbuf_printf(out, "null");
	}
}

static const char *kind_name(enum kind kind) {
	static const char *const names[] = {"int", "float", "bool", "color", "string", "enum"};
	return names[kind];
}

static void write_schema(struct strbuf *out, const struct setting *s) {
	strbuf_printf(out, "{\"key\":");
	strbuf_json_string(out, s->key);
	strbuf_printf(out, ",\"type\":\"%s\"", kind_name(s->kind));
	if (s->kind == KIND_INT || s->kind == KIND_FLOAT) {
		strbuf_printf(out, ",\"min\":%g,\"max\":%g", s->min, s->max);
	}
	if (s->names) {
		strbuf_printf(out, ",\"options\":[");
		for (int i = 0; s->names[i]; i++) {
			strbuf_printf(out, "%s", i ? "," : "");
			strbuf_json_string(out, s->names[i]);
		}
		strbuf_printf(out, "]");
	}
	strbuf_printf(out, "}");
}

void settings_write(struct flame_server *server, struct strbuf *out) {
	const struct flame_config *config = server->config;
	strbuf_printf(out, "{\"ok\":true,\"values\":{");
	for (size_t i = 0; i < sizeof(settings) / sizeof(settings[0]); i++) {
		strbuf_printf(out, "%s", i ? "," : "");
		strbuf_json_string(out, settings[i].key);
		strbuf_printf(out, ":");
		write_value(out, config, settings[i].key);
	}
	strbuf_printf(out, "},\"pinned\":[");
	for (size_t i = 0; i < config->pinned_count; i++) {
		strbuf_printf(out, "%s", i ? "," : "");
		strbuf_json_string(out, config->pinned[i]);
	}
	strbuf_printf(out, "],\"schema\":[");
	for (size_t i = 0; i < sizeof(settings) / sizeof(settings[0]); i++) {
		strbuf_printf(out, "%s", i ? "," : "");
		write_schema(out, &settings[i]);
	}
	strbuf_printf(out, ",");
	write_schema(out, &output_scale);
	strbuf_printf(out, "],\"outputs\":[");
	struct flame_output *output;
	bool first = true;
	wl_list_for_each(output, &server->outputs.list, link) {
		struct wlr_output *wlr = output->wlr;
		strbuf_printf(out, "%s{\"name\":", first ? "" : ",");
		strbuf_json_string(out, wlr->name);
		strbuf_printf(out, ",\"description\":");
		strbuf_json_string(out, wlr->description);
		strbuf_printf(out, ",\"width\":%d,\"height\":%d,\"refresh\":%.2f,\"scale\":%.2f"
			",\"vrr_capable\":%s}", wlr->width, wlr->height, wlr->refresh / 1000.0,
			wlr->scale, wlr->adaptive_sync_supported ? "true" : "false");
		first = false;
	}
	strbuf_printf(out, "],\"settings_file\":");
	strbuf_json_string(out, config_settings_path());
	strbuf_printf(out, ",\"init_file\":");
	strbuf_json_string(out, server->config_path ? server->config_path : config_default_path());
	strbuf_printf(out, "}");
}

// ---- values in ----

static bool true_word(const char *v, bool *out) {
	if (!strcmp(v, "true") || !strcmp(v, "on") || !strcmp(v, "yes") || !strcmp(v, "1")) {
		*out = true;
		return true;
	}
	if (!strcmp(v, "false") || !strcmp(v, "off") || !strcmp(v, "no") || !strcmp(v, "0")) {
		*out = false;
		return true;
	}
	return false;
}

static bool color_word(const char *v) {
	size_t len = strlen(v);
	if (v[0] != '#' || (len != 7 && len != 9)) {
		return false;
	}
	for (size_t i = 1; i < len; i++) {
		if (!strchr("0123456789abcdefABCDEF", v[i])) {
			return false;
		}
	}
	return true;
}

// value as a lua literal, checked against what the setting takes
static bool literal(const struct setting *s, const char *value, struct strbuf *out,
		const char **error) {
	char *end;
	switch (s->kind) {
	case KIND_INT:;
		long n = strtol(value, &end, 10);
		if (*value == '\0' || *end || n < s->min || n > s->max) {
			*error = "not a whole number in range";
			return false;
		}
		strbuf_printf(out, "%ld", n);
		return true;
	case KIND_FLOAT:;
		double d = strtod(value, &end);
		if (*value == '\0' || *end || !(d >= s->min && d <= s->max)) {
			*error = "not a number in range";
			return false;
		}
		strbuf_printf(out, "%g", d);
		return true;
	case KIND_BOOL:;
		bool b;
		if (!true_word(value, &b)) {
			*error = "not true or false";
			return false;
		}
		strbuf_printf(out, "%s", b ? "true" : "false");
		return true;
	case KIND_COLOR:
		if (!color_word(value)) {
			*error = "not a color, #rrggbb or #rrggbbaa";
			return false;
		}
		strbuf_printf(out, "\"%s\"", value);
		return true;
	case KIND_ENUM:
		for (int i = 0; s->names[i]; i++) {
			if (!strcmp(s->names[i], value)) {
				strbuf_printf(out, "\"%s\"", value);
				return true;
			}
		}
		*error = "not one of its options";
		return false;
	case KIND_STRING:
		strbuf_printf(out, "\"");
		for (const char *p = value; *p; p++) {
			if (*p == '"' || *p == '\\') {
				strbuf_printf(out, "\\%c", *p);
			} else if ((unsigned char)*p < 0x20) {
				*error = "no control characters";
				return false;
			} else {
				strbuf_append(out, p, 1);
			}
		}
		strbuf_printf(out, "\"");
		return true;
	}
	return false;
}

// the line that sets key to lit, its key after it
static void line_for(struct strbuf *out, const char *key, const char *output, const char *lit) {
	if (*output) {
		strbuf_printf(out, "flame.output(\"%s\", { scale = %s }) -- %s\n", output, lit, key);
	} else if (!strncmp(key, "keyboard.", 9) || !strncmp(key, "animations.", 11)) {
		const char *dot = strchr(key, '.');
		strbuf_printf(out, "flame.set{ %.*s = { %s = %s } } -- %s\n", (int)(dot - key), key,
			dot + 1, lit, key);
	} else {
		strbuf_printf(out, "flame.set{ %s = %s } -- %s\n", key, lit, key);
	}
}

// the file's lines with key's replaced by line (NULL: dropped), written
// whole to a new file moved over the old one
static bool rewrite(const char *key, const char *line, const char **error) {
	const char *path = config_settings_path();
	char marker[192];
	snprintf(marker, sizeof(marker), " -- %s", key);
	struct strbuf out;
	strbuf_init(&out);
	FILE *in = fopen(path, "r");
	bool done = false;
	if (in) {
		char *text = NULL;
		size_t size = 0;
		ssize_t got;
		while ((got = getline(&text, &size, in)) >= 0) {
			size_t len = (size_t)got;
			while (len && (text[len - 1] == '\n' || text[len - 1] == '\r')) {
				len--;
			}
			size_t mlen = strlen(marker);
			bool ours = len >= mlen && !strncmp(text + len - mlen, marker, mlen);
			if (!ours) {
				strbuf_append(&out, text, len);
				strbuf_printf(&out, "\n");
			} else if (line && !done) {
				strbuf_printf(&out, "%s", line);
				done = true;
			}
		}
		free(text);
		fclose(in);
	} else {
		strbuf_printf(&out, "-- what flamesettings (or flamectl set) changed. init.lua next to it\n"
			"-- wins over anything here\n");
	}
	if (line && !done) {
		strbuf_printf(&out, "%s", line);
	}
	// its directory may not be there yet
	char dir[512];
	snprintf(dir, sizeof(dir), "%s", path);
	char *slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
		mkdir(dir, 0755);
	}
	char tmp[600];
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	FILE *file = out.failed ? NULL : fopen(tmp, "w");
	bool ok = file && fwrite(out.data ? out.data : "", 1, out.len, file) == out.len;
	ok = file && fclose(file) == 0 && ok;
	ok = ok && rename(tmp, path) == 0;
	if (!ok) {
		unlink(tmp);
		*error = "settings.lua can't be written";
	}
	strbuf_finish(&out);
	return ok;
}

bool settings_set(struct flame_server *server, const char *key, const char *value,
		const char **error) {
	char output[128] = "";
	const struct setting *s = find(key, output, sizeof(output));
	if (!s) {
		*error = "no such setting";
		return false;
	}
	if (pinned(server->config, key)) {
		*error = "init.lua sets it, it wins";
		return false;
	}
	struct strbuf lit, line;
	strbuf_init(&lit);
	strbuf_init(&line);
	bool ok = literal(s, value, &lit, error);
	// a wallpaper is a picture flame reads, or "" for none
	if (ok && !strcmp(key, "wallpaper") && *value
			&& (value[0] != '/' || image_sniff(value) == IMAGE_NONE)) {
		*error = value[0] != '/' ? "not a full path" : "not a jpeg, png or webp it can read";
		ok = false;
	}
	if (ok) {
		line_for(&line, key, output, lit.data ? lit.data : "");
		ok = !line.failed && rewrite(key, line.data, error);
	}
	strbuf_finish(&lit);
	strbuf_finish(&line);
	if (ok) {
		server_reload_config(server);
	}
	return ok;
}

bool settings_unset(struct flame_server *server, const char *key, const char **error) {
	char output[128] = "";
	if (!find(key, output, sizeof(output))) {
		*error = "no such setting";
		return false;
	}
	if (!rewrite(key, NULL, error)) {
		return false;
	}
	server_reload_config(server);
	return true;
}
