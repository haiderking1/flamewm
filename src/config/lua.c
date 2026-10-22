#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <strings.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

#include "bindings/action.h"
#include "config/config.h"
#include "core/server.h"
#include "util/spawn.h"

// init.lua: flame's config is lua. it starts from the compiled in defaults
// and changes what it says. the api, all under the global `flame`:
//   flame.set{ gap = 6, border_width = 2, border_color = "#2a2a2a",
//     border_focused_color = "#ff6a00", background = "#111111",
//     repeat_rate = 30, repeat_delay = 250, focus_follows_mouse = true,
//     x_zero_scaling = false, x_global_keys = "always" ("never", "modifiers",
//     "combos": X apps see keys typed in wayland windows, push to talk),
//     allow_tearing = true (fullscreen games with vsync off tear, if they ask),
//     vrr = "fullscreen" ("off", "always": adaptive sync where the screen can),
//     pace = "vrr" ("off", "always": the fullscreen game's frames at an even
//     pace, the fastest it holds), pace_fps = 0 (a cap, 0 for none),
//     drag_modifier = "super", cursor_theme = "Adwaita", cursor_size = 24,
//     wallpaper = "~/walls/sea.jpg" ("" for none: jpeg, png or webp),
//     wallpaper_mode = "fill" ("fit", "center", "stretch"),
//     keyboard = { layout = "us,ara", variant = "", model = "",
//       options = "grp:alt_shift_toggle", rules = "" } }
//   flame.bind(mods, key, action)  mods "super+shift", key a keysym name,
//     action a function or a string to spawn. replaces a bind on the same keys
//   flame.unbind(mods, key), flame.clear_binds()
//   flame.set{ animations = false } or animations = { enabled, layers,
//     open, close, layer_open, layer_close ("slide", "zoom", "fade", "none"),
//     fade_in, fade_out, fade_in_from, fade_out_from, zoom_from, zoom_to,
//     workspace_vertical, focused_opacity, unfocused_opacity,
//     duration = { open, close, move, workspace, focus } (ms),
//     curve = { open, close, move, workspace, focus, fade_in, fade_out }
//       ({x1, y1, x2, y2}, a css cubic-bezier) }
//   flame.rule(match, props)  match { app_id = pattern, title = pattern }
//     (lua patterns), props { floating = bool, workspace = n, fullscreen = bool,
//     animation = false, open = "zoom", close = "fade", fade_in = false,
//     fade_out = false, opacity = 0.9 }
//   flame.autostart(command)  run once when flame comes up
//   flame.output(name, { scale = 1.5 })  name a connector (DP-1) or "*"
//   flame.spawn(command), flame.quit(), flame.close(), flame.fullscreen(),
//   flame.toggle_floating(), flame.focus("next"|"prev"), flame.workspace(n),
//   flame.move_to_workspace(n), flame.reload(), flame.log(message)

struct flame_lua {
	lua_State *L;
	struct flame_server *server; // NULL while the first load runs
	struct flame_config *config;
	size_t binds_cap;
	// spawn binds' command lines, freed with it
	char ***argvs;
	size_t argvs_len, argvs_cap;
	// init.lua running: what it sets is pinned, settings (flamesettings)
	// can't change it, see config->pinned
	bool pinning;
};

// key ("gap", "keyboard.layout", "output.DP-1.scale") pinned by init.lua
static void pin(struct flame_lua *lua, const char *key) {
	struct flame_config *config = lua->config;
	if (!lua->pinning) {
		return;
	}
	for (size_t i = 0; i < config->pinned_count; i++) {
		if (!strcmp(config->pinned[i], key)) {
			return;
		}
	}
	char *copy = strdup(key);
	char **grown = copy ? realloc(config->pinned, (config->pinned_count + 1) * sizeof(*grown))
		: NULL;
	if (!grown) {
		free(copy);
		return;
	}
	config->pinned = grown;
	config->pinned[config->pinned_count++] = copy;
}

// a table's fields, each pinned as "prefix.field"
static void pin_fields(lua_State *L, struct flame_lua *lua, int table, const char *prefix) {
	if (!lua->pinning) {
		return;
	}
	lua_pushnil(L);
	while (lua_next(L, table)) {
		if (lua_type(L, -2) == LUA_TSTRING) {
			char key[128];
			snprintf(key, sizeof(key), "%s.%s", prefix, lua_tostring(L, -2));
			pin(lua, key);
		}
		lua_pop(L, 1);
	}
}

static const char prelude[] =
	"local rules = {}\n"
	"function flame.rule(match, props)\n"
	"  assert(type(match) == 'table' and type(props) == 'table', 'flame.rule(match, props)')\n"
	"  rules[#rules + 1] = { match = match, props = props }\n"
	"end\n"
	"function flame._rules(app_id, title)\n"
	"  local out = {}\n"
	"  for _, rule in ipairs(rules) do\n"
	"    local fits = true\n"
	"    for key, pattern in pairs(rule.match) do\n"
	"      local value = key == 'app_id' and app_id or key == 'title' and title or nil\n"
	"      if not value or not string.find(value, pattern) then fits = false break end\n"
	"    end\n"
	"    if fits then for k, v in pairs(rule.props) do out[k] = v end end\n"
	"  end\n"
	"  return out\n"
	"end\n";

static struct flame_lua *self(lua_State *L) {
	return lua_touserdata(L, lua_upvalueindex(1));
}

// actions run against a server, there's none while the first load runs
static struct flame_server *server_or_warn(lua_State *L, const char *what) {
	struct flame_lua *lua = self(L);
	if (!lua->server) {
		wlr_log(WLR_ERROR, "config: %s can't run while the config loads", what);
	}
	return lua->server;
}

// ---- values ----

static bool parse_color(const char *text, float out[4]) {
	if (!text || text[0] != '#') {
		return false;
	}
	size_t len = strlen(text + 1);
	if (len != 6 && len != 8) {
		return false;
	}
	char *end;
	unsigned long v = strtoul(text + 1, &end, 16);
	if (*end) {
		return false;
	}
	if (len == 6) {
		v = v << 8 | 0xff;
	}
	float a = (float)(v & 0xff) / 255.0f;
	// premultiplied, what the scene wants
	out[0] = (float)(v >> 24 & 0xff) / 255.0f * a;
	out[1] = (float)(v >> 16 & 0xff) / 255.0f * a;
	out[2] = (float)(v >> 8 & 0xff) / 255.0f * a;
	out[3] = a;
	return true;
}

static bool parse_mods(const char *text, uint32_t *out) {
	*out = 0;
	char buf[128];
	snprintf(buf, sizeof(buf), "%s", text ? text : "");
	char *save = NULL;
	for (char *word = strtok_r(buf, "+ ", &save); word; word = strtok_r(NULL, "+ ", &save)) {
		if (!strcasecmp(word, "super") || !strcasecmp(word, "logo") || !strcasecmp(word, "mod4")) {
			*out |= WLR_MODIFIER_LOGO;
		} else if (!strcasecmp(word, "shift")) {
			*out |= WLR_MODIFIER_SHIFT;
		} else if (!strcasecmp(word, "ctrl") || !strcasecmp(word, "control")) {
			*out |= WLR_MODIFIER_CTRL;
		} else if (!strcasecmp(word, "alt") || !strcasecmp(word, "mod1")) {
			*out |= WLR_MODIFIER_ALT;
		} else if (!strcasecmp(word, "altgr") || !strcasecmp(word, "mod5")) {
			*out |= WLR_MODIFIER_MOD5;
		} else {
			return false;
		}
	}
	return true;
}

static char *dup_field(lua_State *L, int table, const char *key) {
	lua_getfield(L, table, key);
	char *value = lua_isstring(L, -1) ? strdup(lua_tostring(L, -1)) : NULL;
	lua_pop(L, 1);
	return value;
}

static float clamp01(float v) {
	return v < 0 ? 0 : v > 1 ? 1 : v;
}

static bool parse_anim_type(const char *text, enum flame_anim_type *out) {
	static const char *const names[] = {"none", "slide", "zoom", "fade"};
	for (int i = 0; text && i < 4; i++) {
		if (!strcmp(text, names[i])) {
			*out = (enum flame_anim_type)i;
			return true;
		}
	}
	return false;
}

// {x1, y1, x2, y2} or "x1,y1,x2,y2", like css cubic-bezier
static bool parse_curve(lua_State *L, int index, struct flame_curve *out) {
	double v[4];
	if (lua_istable(L, index)) {
		for (int i = 0; i < 4; i++) {
			lua_rawgeti(L, index, i + 1);
			if (!lua_isnumber(L, -1)) {
				lua_pop(L, 1);
				return false;
			}
			v[i] = lua_tonumber(L, -1);
			lua_pop(L, 1);
		}
	} else if (!lua_isstring(L, index)
			|| sscanf(lua_tostring(L, index), "%lf,%lf,%lf,%lf", &v[0], &v[1], &v[2], &v[3])
				!= 4) {
		return false;
	}
	*out = (struct flame_curve){v[0], v[1], v[2], v[3]};
	return true;
}

static int set_animations(lua_State *L, struct flame_anim_config *anim, int table) {
	lua_pushnil(L);
	while (lua_next(L, table)) {
		const char *key = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : "";
		int value = lua_gettop(L);
		bool ok = true;
		if (!strcmp(key, "enabled")) {
			anim->enabled = lua_toboolean(L, value);
		} else if (!strcmp(key, "layers")) {
			anim->layers = lua_toboolean(L, value);
		} else if (!strcmp(key, "open")) {
			ok = parse_anim_type(lua_tostring(L, value), &anim->open);
		} else if (!strcmp(key, "close")) {
			ok = parse_anim_type(lua_tostring(L, value), &anim->close);
		} else if (!strcmp(key, "layer_open")) {
			ok = parse_anim_type(lua_tostring(L, value), &anim->layer_open);
		} else if (!strcmp(key, "layer_close")) {
			ok = parse_anim_type(lua_tostring(L, value), &anim->layer_close);
		} else if (!strcmp(key, "fade_in")) {
			anim->fade_in = lua_toboolean(L, value);
		} else if (!strcmp(key, "fade_out")) {
			anim->fade_out = lua_toboolean(L, value);
		} else if (!strcmp(key, "fade_in_from")) {
			anim->fade_in_from = clamp01((float)luaL_checknumber(L, value));
		} else if (!strcmp(key, "fade_out_from")) {
			anim->fade_out_from = clamp01((float)luaL_checknumber(L, value));
		} else if (!strcmp(key, "zoom_from")) {
			anim->zoom_from = (float)luaL_checknumber(L, value);
		} else if (!strcmp(key, "zoom_to")) {
			anim->zoom_to = (float)luaL_checknumber(L, value);
		} else if (!strcmp(key, "workspace_vertical")) {
			anim->workspace_vertical = lua_toboolean(L, value);
		} else if (!strcmp(key, "focused_opacity")) {
			anim->focused_opacity = clamp01((float)luaL_checknumber(L, value));
		} else if (!strcmp(key, "unfocused_opacity")) {
			anim->unfocused_opacity = clamp01((float)luaL_checknumber(L, value));
		} else if (!strcmp(key, "duration") || !strcmp(key, "curve")) {
			bool duration = !strcmp(key, "duration");
			luaL_checktype(L, value, LUA_TTABLE);
			static const char *const names[] = {"open", "close", "move", "workspace", "focus",
				"fade_in", "fade_out"};
			int *ms[] = {&anim->open_ms, &anim->close_ms, &anim->move_ms, &anim->workspace_ms,
				&anim->focus_ms, NULL, NULL};
			struct flame_curve *curves[] = {&anim->open_curve, &anim->close_curve,
				&anim->move_curve, &anim->workspace_curve, &anim->focus_curve,
				&anim->fade_in_curve, &anim->fade_out_curve};
			for (int i = 0; i < 7; i++) {
				lua_getfield(L, value, names[i]);
				if (!lua_isnil(L, -1)) {
					if (duration && ms[i]) {
						*ms[i] = (int)luaL_checkinteger(L, -1);
					} else if (!duration && !parse_curve(L, lua_gettop(L), curves[i])) {
						return luaL_error(L, "flame.set: animations.curve.%s is {x1, y1, x2, y2}",
							names[i]);
					}
				}
				lua_pop(L, 1);
			}
		} else {
			return luaL_error(L, "flame.set: animations has no %s", key);
		}
		if (!ok) {
			return luaL_error(L, "flame.set: animations.%s is none, slide, zoom or fade", key);
		}
		lua_pop(L, 1);
	}
	return 0;
}

static void set_keyboard(lua_State *L, struct flame_config *config, int table) {
	struct flame_keyboard_names *k = &config->keyboard;
	char **fields[] = {&k->rules, &k->model, &k->layout, &k->variant, &k->options};
	const char *names[] = {"rules", "model", "layout", "variant", "options"};
	for (int i = 0; i < 5; i++) {
		char *value = dup_field(L, table, names[i]);
		if (value) {
			free(*fields[i]);
			*fields[i] = value;
		}
	}
}

// flame.set{...}
static int l_set(lua_State *L) {
	struct flame_config *config = self(L)->config;
	luaL_checktype(L, 1, LUA_TTABLE);
	lua_pushnil(L);
	while (lua_next(L, 1)) {
		const char *key = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : "";
		int value = lua_gettop(L);
		bool ok = true;
		if (!strcmp(key, "gap")) {
			config->gap = (int)luaL_checkinteger(L, value);
		} else if (!strcmp(key, "border_width")) {
			config->border_width = (int)luaL_checkinteger(L, value);
		} else if (!strcmp(key, "repeat_rate")) {
			config->repeat_rate = (int32_t)luaL_checkinteger(L, value);
		} else if (!strcmp(key, "repeat_delay")) {
			config->repeat_delay = (int32_t)luaL_checkinteger(L, value);
		} else if (!strcmp(key, "cursor_size")) {
			config->cursor_size = (uint32_t)luaL_checkinteger(L, value);
		} else if (!strcmp(key, "focus_follows_mouse")) {
			config->focus_follows_mouse = lua_toboolean(L, value);
		} else if (!strcmp(key, "x_zero_scaling")) {
			config->x_zero_scaling = lua_toboolean(L, value);
		} else if (!strcmp(key, "allow_tearing")) {
			config->allow_tearing = lua_toboolean(L, value);
		} else if (!strcmp(key, "vrr")) {
			ok = lua_isstring(L, value)
				&& sync_vrr_policy_parse(lua_tostring(L, value), &config->vrr);
		} else if (!strcmp(key, "pace")) {
			ok = lua_isstring(L, value)
				&& pacer_mode_parse(lua_tostring(L, value), &config->pace);
		} else if (!strcmp(key, "pace_fps")) {
			lua_Integer fps = luaL_checkinteger(L, value);
			ok = fps >= 0 && fps <= 1000;
			if (ok) {
				config->pace_fps = (int)fps;
			}
		} else if (!strcmp(key, "x_global_keys")) {
			static const char *const names[] = {"never", "modifiers", "combos", "always"};
			const char *name = lua_tostring(L, value);
			ok = false;
			for (int i = 0; name && i < 4; i++) {
				if (!strcmp(name, names[i])) {
					config->x_global_keys = (enum flame_x_global_keys)i;
					ok = true;
				}
			}
		} else if (!strcmp(key, "border_color")) {
			ok = parse_color(lua_tostring(L, value), config->border_color);
		} else if (!strcmp(key, "border_focused_color")) {
			ok = parse_color(lua_tostring(L, value), config->border_focused_color);
		} else if (!strcmp(key, "background")) {
			ok = parse_color(lua_tostring(L, value), config->background_color);
		} else if (!strcmp(key, "drag_modifier")) {
			ok = parse_mods(lua_tostring(L, value), &config->drag_modifier);
		} else if (!strcmp(key, "wallpaper")) {
			// "" for none, ~ for home
			const char *path = lua_isstring(L, value) ? lua_tostring(L, value) : "";
			const char *home = getenv("HOME");
			free(config->wallpaper);
			config->wallpaper = NULL;
			if (!strncmp(path, "~/", 2) && home) {
				size_t len = strlen(home) + strlen(path);
				config->wallpaper = malloc(len);
				if (config->wallpaper) {
					snprintf(config->wallpaper, len, "%s%s", home, path + 1);
				}
			} else if (*path) {
				config->wallpaper = strdup(path);
			}
		} else if (!strcmp(key, "wallpaper_mode")) {
			static const char *const modes[] = {"fill", "fit", "center", "stretch"};
			const char *mode = lua_isstring(L, value) ? lua_tostring(L, value) : "";
			ok = false;
			for (int i = 0; i < 4; i++) {
				if (!strcmp(mode, modes[i])) {
					config->wallpaper_mode = (enum flame_wallpaper_mode)i;
					ok = true;
				}
			}
		} else if (!strcmp(key, "cursor_theme")) {
			free(config->owned_cursor_theme);
			config->owned_cursor_theme = lua_isstring(L, value) ? strdup(lua_tostring(L, value))
				: NULL;
			config->cursor_theme = config->owned_cursor_theme;
		} else if (!strcmp(key, "keyboard")) {
			luaL_checktype(L, value, LUA_TTABLE);
			set_keyboard(L, config, value);
			pin_fields(L, self(L), value, "keyboard");
		} else if (!strcmp(key, "animations")) {
			if (lua_isboolean(L, value)) {
				config->anim.enabled = lua_toboolean(L, value);
				config->anim.layers = config->anim.enabled;
				pin(self(L), "animations.enabled");
			} else {
				luaL_checktype(L, value, LUA_TTABLE);
				set_animations(L, &config->anim, value);
				pin_fields(L, self(L), value, "animations");
			}
		} else {
			return luaL_error(L, "flame.set: there's no %s", key);
		}
		if (!ok) {
			return luaL_error(L, "flame.set: %s doesn't make sense as %s",
				lua_tostring(L, value), key);
		}
		if (strcmp(key, "keyboard") && strcmp(key, "animations")) {
			pin(self(L), key);
		}
		lua_pop(L, 1);
	}
	return 0;
}

// ---- keybinds ----

static struct flame_keybind *find_bind(struct flame_config *config, uint32_t mods,
		xkb_keysym_t sym) {
	for (size_t i = 0; i < config->keybind_count; i++) {
		if (config->owned_keybinds[i].mods == mods && config->owned_keybinds[i].sym == sym) {
			return &config->owned_keybinds[i];
		}
	}
	return NULL;
}

static void release_bind(struct flame_lua *lua, struct flame_keybind *bind) {
	if (bind->action == action_lua) {
		luaL_unref(lua->L, LUA_REGISTRYINDEX, bind->arg.i);
	}
}

static void check_keys(lua_State *L, uint32_t *mods, xkb_keysym_t *sym) {
	if (!parse_mods(luaL_checkstring(L, 1), mods)) {
		luaL_error(L, "flame.bind: what modifiers are %s", lua_tostring(L, 1));
	}
	const char *key = luaL_checkstring(L, 2);
	*sym = xkb_keysym_from_name(key, XKB_KEYSYM_NO_FLAGS);
	if (*sym == XKB_KEY_NoSymbol) {
		*sym = xkb_keysym_from_name(key, XKB_KEYSYM_CASE_INSENSITIVE);
	}
	if (*sym == XKB_KEY_NoSymbol) {
		luaL_error(L, "flame.bind: there's no key %s", key);
	}
	*sym = xkb_keysym_to_lower(*sym);
}

static const char *const *spawn_argv(struct flame_lua *lua, const char *command) {
	if (lua->argvs_len == lua->argvs_cap) {
		size_t cap = lua->argvs_cap ? lua->argvs_cap * 2 : 16;
		char ***grown = realloc(lua->argvs, cap * sizeof(*grown));
		if (!grown) {
			return NULL;
		}
		lua->argvs = grown;
		lua->argvs_cap = cap;
	}
	char **argv = calloc(4, sizeof(*argv));
	if (!argv) {
		return NULL;
	}
	argv[0] = strdup("/bin/sh");
	argv[1] = strdup("-c");
	argv[2] = strdup(command);
	lua->argvs[lua->argvs_len++] = argv;
	return (const char *const *)argv;
}

// flame.bind(mods, key, action)
static int l_bind(lua_State *L) {
	struct flame_lua *lua = self(L);
	struct flame_config *config = lua->config;
	uint32_t mods;
	xkb_keysym_t sym;
	check_keys(L, &mods, &sym);
	struct flame_keybind bind = {.mods = mods, .sym = sym};
	if (lua_isfunction(L, 3)) {
		lua_pushvalue(L, 3);
		bind.action = action_lua;
		bind.arg.i = luaL_ref(L, LUA_REGISTRYINDEX);
	} else if (lua_isstring(L, 3)) {
		bind.action = action_spawn;
		bind.arg.argv = spawn_argv(lua, lua_tostring(L, 3));
		if (!bind.arg.argv) {
			return luaL_error(L, "flame.bind: out of memory");
		}
	} else {
		return luaL_error(L, "flame.bind: the action is a function or a command");
	}
	struct flame_keybind *existing = find_bind(config, mods, sym);
	if (existing) {
		release_bind(lua, existing);
		*existing = bind;
		return 0;
	}
	if (config->keybind_count == lua->binds_cap) {
		size_t cap = lua->binds_cap * 2;
		struct flame_keybind *grown = realloc(config->owned_keybinds, cap * sizeof(*grown));
		if (!grown) {
			return luaL_error(L, "flame.bind: out of memory");
		}
		config->owned_keybinds = grown;
		config->keybinds = grown;
		lua->binds_cap = cap;
	}
	config->owned_keybinds[config->keybind_count++] = bind;
	return 0;
}

static int l_unbind(lua_State *L) {
	struct flame_lua *lua = self(L);
	struct flame_config *config = lua->config;
	uint32_t mods;
	xkb_keysym_t sym;
	check_keys(L, &mods, &sym);
	struct flame_keybind *bind = find_bind(config, mods, sym);
	if (bind) {
		release_bind(lua, bind);
		size_t at = (size_t)(bind - config->owned_keybinds);
		memmove(bind, bind + 1, (config->keybind_count - at - 1) * sizeof(*bind));
		config->keybind_count--;
	}
	return 0;
}

static int l_clear_binds(lua_State *L) {
	struct flame_lua *lua = self(L);
	for (size_t i = 0; i < lua->config->keybind_count; i++) {
		release_bind(lua, &lua->config->owned_keybinds[i]);
	}
	lua->config->keybind_count = 0;
	return 0;
}

// ---- actions ----

static int l_autostart(lua_State *L) {
	struct flame_config *config = self(L)->config;
	const char *command = luaL_checkstring(L, 1);
	char **grown = realloc(config->autostart, (config->autostart_count + 1) * sizeof(*grown));
	if (!grown) {
		return luaL_error(L, "flame.autostart: out of memory");
	}
	config->autostart = grown;
	config->autostart[config->autostart_count++] = strdup(command);
	return 0;
}

static int l_output(lua_State *L) {
	struct flame_config *config = self(L)->config;
	const char *name = luaL_checkstring(L, 1);
	luaL_checktype(L, 2, LUA_TTABLE);
	lua_getfield(L, 2, "scale");
	float scale = 0;
	if (!lua_isnil(L, -1)) {
		lua_Number n = luaL_checknumber(L, -1);
		if (!(n >= 0.25 && n <= 8)) {
			return luaL_error(L, "flame.output: scale %f is out of 0.25..8", n);
		}
		scale = (float)n;
	}
	lua_pop(L, 1);
	// the same name again replaces it
	struct flame_output_config *out = NULL;
	for (size_t i = 0; i < config->output_count; i++) {
		if (!strcmp(config->outputs[i].name, name)) {
			out = &config->outputs[i];
		}
	}
	if (!out) {
		char *copy = strdup(name);
		struct flame_output_config *grown = copy ? realloc(config->outputs,
			(config->output_count + 1) * sizeof(*grown)) : NULL;
		if (!grown) {
			free(copy);
			return luaL_error(L, "flame.output: out of memory");
		}
		config->outputs = grown;
		out = &config->outputs[config->output_count++];
		out->name = copy;
	}
	out->scale = scale;
	char key[160];
	snprintf(key, sizeof(key), "output.%s.scale", name);
	pin(self(L), key);
	return 0;
}

static int l_spawn(lua_State *L) {
	const char *argv[] = {"/bin/sh", "-c", luaL_checkstring(L, 1), NULL};
	spawn(argv);
	return 0;
}

static int l_log(lua_State *L) {
	wlr_log(WLR_INFO, "config: %s", luaL_checkstring(L, 1));
	return 0;
}

static int run_action(lua_State *L, const char *name, flame_action_fn action,
		union flame_arg arg) {
	struct flame_server *server = server_or_warn(L, name);
	if (server) {
		action(server, &arg);
	}
	return 0;
}

static int l_quit(lua_State *L) {
	return run_action(L, "flame.quit", action_quit, (union flame_arg){0});
}

static int l_close(lua_State *L) {
	return run_action(L, "flame.close", action_close, (union flame_arg){0});
}

static int l_fullscreen(lua_State *L) {
	return run_action(L, "flame.fullscreen", action_fullscreen, (union flame_arg){0});
}

static int l_toggle_floating(lua_State *L) {
	return run_action(L, "flame.toggle_floating", action_toggle_floating,
		(union flame_arg){0});
}

static int l_reload(lua_State *L) {
	return run_action(L, "flame.reload", action_reload, (union flame_arg){0});
}

static int l_focus(lua_State *L) {
	int direction = 1;
	if (lua_isstring(L, 1) && !lua_isnumber(L, 1)) {
		direction = !strcmp(lua_tostring(L, 1), "prev") ? -1 : 1;
	} else if (lua_isnumber(L, 1)) {
		direction = lua_tointeger(L, 1) < 0 ? -1 : 1;
	}
	return run_action(L, "flame.focus", action_focus, (union flame_arg){.i = direction});
}

static int workspace_arg(lua_State *L) {
	lua_Integer n = luaL_checkinteger(L, 1);
	if (n < 1 || n > FLAME_WORKSPACE_COUNT) {
		return luaL_error(L, "workspaces are 1-%d", FLAME_WORKSPACE_COUNT);
	}
	return (int)n - 1;
}

static int l_workspace(lua_State *L) {
	return run_action(L, "flame.workspace", action_workspace,
		(union flame_arg){.i = workspace_arg(L)});
}

static int l_move_to_workspace(lua_State *L) {
	return run_action(L, "flame.move_to_workspace", action_move_to_workspace,
		(union flame_arg){.i = workspace_arg(L)});
}

static const luaL_Reg functions[] = {
	{"set", l_set},
	{"bind", l_bind},
	{"unbind", l_unbind},
	{"clear_binds", l_clear_binds},
	{"autostart", l_autostart},
	{"output", l_output},
	{"spawn", l_spawn},
	{"log", l_log},
	{"quit", l_quit},
	{"close", l_close},
	{"fullscreen", l_fullscreen},
	{"toggle_floating", l_toggle_floating},
	{"reload", l_reload},
	{"focus", l_focus},
	{"workspace", l_workspace},
	{"move_to_workspace", l_move_to_workspace},
	{NULL, NULL},
};

// ---- loading ----

static void config_file(char *path, size_t size, const char *name) {
	const char *xdg = getenv("XDG_CONFIG_HOME");
	if (xdg && *xdg) {
		snprintf(path, size, "%s/flame/%s", xdg, name);
	} else {
		snprintf(path, size, "%s/.config/flame/%s", getenv("HOME") ? getenv("HOME") : "", name);
	}
}

const char *config_default_path(void) {
	static char path[512];
	config_file(path, sizeof(path), "init.lua");
	return path;
}

const char *config_settings_path(void) {
	static char path[512];
	const char *env = getenv("FLAME_SETTINGS");
	if (env && *env) {
		snprintf(path, sizeof(path), "%s", env);
	} else {
		config_file(path, sizeof(path), "settings.lua");
	}
	return path;
}

static void lua_free(struct flame_lua *lua) {
	if (!lua) {
		return;
	}
	if (lua->L) {
		lua_close(lua->L);
	}
	for (size_t i = 0; i < lua->argvs_len; i++) {
		for (char **arg = lua->argvs[i]; *arg; arg++) {
			free(*arg);
		}
		free(lua->argvs[i]);
	}
	free(lua->argvs);
	free(lua);
}

void config_free(struct flame_config *config) {
	if (!config) {
		return;
	}
	lua_free(config->lua);
	free(config->owned_keybinds);
	free(config->owned_cursor_theme);
	free(config->wallpaper);
	struct flame_keyboard_names *k = &config->keyboard;
	free(k->rules);
	free(k->model);
	free(k->layout);
	free(k->variant);
	free(k->options);
	for (size_t i = 0; i < config->autostart_count; i++) {
		free(config->autostart[i]);
	}
	free(config->autostart);
	for (size_t i = 0; i < config->output_count; i++) {
		free(config->outputs[i].name);
	}
	free(config->outputs);
	for (size_t i = 0; i < config->pinned_count; i++) {
		free(config->pinned[i]);
	}
	free(config->pinned);
	free(config);
}

const struct flame_output_config *config_output(const struct flame_config *config,
		const char *name) {
	const struct flame_output_config *any = NULL;
	for (size_t i = 0; i < config->output_count; i++) {
		if (!strcmp(config->outputs[i].name, name)) {
			return &config->outputs[i];
		}
		if (!strcmp(config->outputs[i].name, "*")) {
			any = &config->outputs[i];
		}
	}
	return any;
}

static char *message(const char *fmt, const char *a, const char *b) {
	char buf[1024];
	snprintf(buf, sizeof(buf), fmt, a, b);
	return strdup(buf);
}

struct flame_config *config_load(struct flame_server *server, const char *path, char **error) {
	*error = NULL;
	struct flame_config *config = calloc(1, sizeof(*config));
	if (!config) {
		return NULL;
	}
	*config = flame_default_config;
	config->owned_keybinds = calloc(flame_default_config.keybind_count + 16,
		sizeof(*config->owned_keybinds));
	struct flame_lua *lua = calloc(1, sizeof(*lua));
	if (!config->owned_keybinds || !lua) {
		free(lua);
		config_free(config);
		return NULL;
	}
	memcpy(config->owned_keybinds, flame_default_config.keybinds,
		flame_default_config.keybind_count * sizeof(*config->owned_keybinds));
	config->keybinds = config->owned_keybinds;
	config->cursor_theme = NULL;
	lua->binds_cap = flame_default_config.keybind_count + 16;
	lua->config = config;
	config->lua = lua;

	// settings.lua (what flamesettings saves) first, init.lua over it. no
	// files: the defaults, nothing to complain about
	path = path ? path : config_default_path();
	const char *settings = config_settings_path();
	bool has_init = access(path, R_OK) == 0, has_settings = access(settings, R_OK) == 0;
	if (!has_init && !has_settings) {
		return config;
	}

	lua->L = luaL_newstate();
	if (!lua->L) {
		*error = strdup("no memory for lua");
		return config;
	}
	lua_State *L = lua->L;
	luaL_openlibs(L);
	lua_newtable(L);
	lua_pushlightuserdata(L, lua);
	luaL_setfuncs(L, functions, 1);
	lua_setglobal(L, "flame");
	if (luaL_dostring(L, prelude) != LUA_OK) {
		*error = message("config: %s%s", lua_tostring(L, -1), "");
		return config;
	}
	// what got set before an error stays, the rest is defaults
	if (has_settings && luaL_dofile(L, settings) != LUA_OK) {
		*error = message("%s%s", lua_tostring(L, -1), "");
		lua_pop(L, 1);
	}
	lua->pinning = true;
	if (has_init && luaL_dofile(L, path) != LUA_OK) {
		free(*error);
		*error = message("%s%s", lua_tostring(L, -1), "");
		lua_pop(L, 1);
	}
	lua->pinning = false;
	lua->server = server;
	return config;
}

// ---- at runtime ----

void config_window_rules(const struct flame_config *config, const char *app_id,
		const char *title, struct flame_window_rule *out) {
	*out = (struct flame_window_rule){.open = -1, .close = -1};
	// flame's own settings and file chooser float, they can be any size:
	// init.lua's rules can still say otherwise
	if (app_id && (!strcmp(app_id, "flamesettings") || !strcmp(app_id, "flamefiles-chooser"))) {
		out->floating_set = out->floating = true;
	}
	lua_State *L = config->lua ? config->lua->L : NULL;
	if (!L) {
		return;
	}
	int top = lua_gettop(L);
	// init.lua may have done anything to these: nothing unprotected touches
	// a value that isn't what it should be
	if (lua_getglobal(L, "flame") != LUA_TTABLE
			|| lua_rawget(L, (lua_pushstring(L, "_rules"), -2)) != LUA_TFUNCTION) {
		lua_settop(L, top);
		return;
	}
	lua_pushstring(L, app_id ? app_id : "");
	lua_pushstring(L, title ? title : "");
	if (lua_pcall(L, 2, 1, 0) != LUA_OK || !lua_istable(L, -1)) {
		if (!lua_istable(L, -1)) {
			wlr_log(WLR_ERROR, "config: a window rule broke: %s",
				lua_isstring(L, -1) ? lua_tostring(L, -1) : "flame._rules gave no table");
		}
		lua_settop(L, top);
		return;
	}
	lua_pushstring(L, "floating");
	lua_rawget(L, -2);
	if (!lua_isnil(L, -1)) {
		out->floating_set = true;
		out->floating = lua_toboolean(L, -1);
	}
	lua_pop(L, 1);
	lua_pushstring(L, "fullscreen");
	lua_rawget(L, -2);
	out->fullscreen = lua_toboolean(L, -1);
	lua_pop(L, 1);
	lua_pushstring(L, "workspace");
	lua_rawget(L, -2);
	if (lua_isinteger(L, -1)) {
		lua_Integer n = lua_tointeger(L, -1);
		out->workspace = n >= 1 && n <= FLAME_WORKSPACE_COUNT ? (int)n : 0;
	}
	lua_pop(L, 1);
	lua_pushstring(L, "animation");
	lua_rawget(L, -2);
	out->no_animation = lua_isboolean(L, -1) && !lua_toboolean(L, -1);
	lua_pop(L, 1);
	enum flame_anim_type type;
	lua_pushstring(L, "open");
	lua_rawget(L, -2);
	if (lua_isstring(L, -1) && parse_anim_type(lua_tostring(L, -1), &type)) {
		out->open = (int)type;
	}
	lua_pop(L, 1);
	lua_pushstring(L, "close");
	lua_rawget(L, -2);
	if (lua_isstring(L, -1) && parse_anim_type(lua_tostring(L, -1), &type)) {
		out->close = (int)type;
	}
	lua_pop(L, 1);
	lua_pushstring(L, "fade_in");
	lua_rawget(L, -2);
	out->no_fade_in = lua_isboolean(L, -1) && !lua_toboolean(L, -1);
	lua_pop(L, 1);
	lua_pushstring(L, "fade_out");
	lua_rawget(L, -2);
	out->no_fade_out = lua_isboolean(L, -1) && !lua_toboolean(L, -1);
	lua_pop(L, 1);
	lua_pushstring(L, "opacity");
	lua_rawget(L, -2);
	if (lua_isnumber(L, -1)) {
		out->opacity = clamp01((float)lua_tonumber(L, -1));
	}
	lua_settop(L, top);
}

void config_run_lua_bind(struct flame_server *server, int ref) {
	struct flame_lua *lua = server->config->lua;
	if (!lua || !lua->L) {
		return;
	}
	lua_rawgeti(lua->L, LUA_REGISTRYINDEX, ref);
	if (lua_pcall(lua->L, 0, 0, 0) != LUA_OK) {
		wlr_log(WLR_ERROR, "config: a keybind broke: %s", lua_tostring(lua->L, -1));
		lua_pop(lua->L, 1);
	}
}
