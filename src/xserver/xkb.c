#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>

#include "xserver/internal.h"

// the X Keyboard Extension, 1.0: flame's keymap as XKB tells it. toolkits
// (xkbcommon-x11: kitty, alacritty, rofi, qt; gtk through xlib) read the
// whole keymap from it and follow the keyboard's state through StateNotify,
// layouts and all. nothing here changes the keymap, flame owns it: the
// Set requests are accepted and change nothing
//
// xkbcommon doesn't hand out its keymap's insides, so they're worked out:
// key types from which modifiers pick which level, modifier actions by
// pressing each key on a state of its own and seeing what it does

#define MAJOR 1
#define MINOR 0

#define DEVICE_ID 3 // the core keyboard, XI2's virtual core keyboard too
#define BAD_KEYBOARD (XS_XKB_FIRST_ERROR + 0)

// map parts
#define KEY_TYPES 1
#define KEY_SYMS 2
#define MODIFIER_MAP 4
#define EXPLICIT_COMPONENTS 8
#define KEY_ACTIONS 16
#define KEY_BEHAVIORS 32
#define VIRTUAL_MODS 64
#define VIRTUAL_MOD_MAP 128
#define ALL_MAP_PARTS 0xff

// event types
#define NEW_KEYBOARD_NOTIFY 0
#define MAP_NOTIFY 1
#define STATE_NOTIFY 2
#define INDICATOR_STATE_NOTIFY 4

// state parts
#define MODIFIER_STATE 1
#define MODIFIER_BASE 2
#define MODIFIER_LATCH 4
#define MODIFIER_LOCK 8
#define GROUP_STATE 16
#define GROUP_BASE 32
#define GROUP_LATCH 64
#define GROUP_LOCK 128
#define COMPAT_STATE 256
#define GRAB_MODS 512
#define COMPAT_GRAB_MODS 1024
#define LOOKUP_MODS 2048
#define COMPAT_LOOKUP_MODS 4096

// per client flags
#define DETECTABLE_AUTO_REPEAT 1
#define ALL_CLIENT_FLAGS 0x1f

// actions
#define SA_SET_MODS 1
#define SA_LATCH_MODS 2
#define SA_LOCK_MODS 3
#define SA_SET_GROUP 4
#define SA_LATCH_GROUP 5
#define SA_LOCK_GROUP 6
#define SA_CLEAR_LOCKS 1

#define REPEAT_KEYS_CONTROL 1
#define MAX_TYPES 64
#define MAX_ENTRIES 32
#define MAX_GROUPS 4
#define MAX_LEDS 32
#define NUM_VMODS 16

#define SHIFT 1
#define LOCK 2
#define MOD2 16 // num lock, on every keymap anyone uses

struct type {
	uint8_t mask;
	uint8_t levels;
	uint8_t entries_len;
	struct {
		uint8_t mods, level;
	} entries[MAX_ENTRIES];
	const char *name; // NULL: made up from its number
};

struct key {
	char name[4];
	uint8_t groups;
	uint8_t width; // levels, the most any of its groups has
	uint8_t types[MAX_GROUPS];
	uint32_t *syms; // groups * width
	bool repeats;
	// what it does pressed on its own, one action for all its levels
	uint8_t action, action_flags, action_mods;
	int8_t action_group;
	uint8_t modmap; // the real modifiers it sets or locks
};

struct xs_xkb {
	struct xkb_keymap *keymap;
	char *text; // the keymap as text, telling a new keymap from a copy
	uint8_t min, max; // keycodes
	struct type types[MAX_TYPES];
	int types_len;
	struct key keys[256];
	int groups;
	uint32_t group_names[MAX_GROUPS];
	uint16_t vmods; // the virtual modifiers there are
	uint8_t vmod_real[NUM_VMODS];
	uint32_t vmod_names[NUM_VMODS];
	uint32_t leds; // the indicators there are, bit by index
	uint32_t led_names[MAX_LEDS];
	uint8_t led_mods[MAX_LEDS]; // the locked modifiers that light it
	uint32_t keycodes_name, symbols_name, types_name, compat_name;
};

// the four types every XKB keymap starts with, clients count on them
static const struct type canonical[] = {
	{0, 1, 0, {{0}}, "ONE_LEVEL"},
	{SHIFT, 2, 1, {{SHIFT, 1}}, "TWO_LEVEL"},
	{SHIFT | LOCK, 2, 2, {{SHIFT, 1}, {LOCK, 1}}, "ALPHABETIC"},
	{SHIFT | MOD2, 2, 2, {{SHIFT, 1}, {MOD2, 1}}, "KEYPAD"},
};

static uint32_t atom(struct flame_xserver *xserver, const char *name) {
	return name ? xs_atom_intern(&xserver->atoms, name, strlen(name), true) : 0;
}

// ---- working the keymap out ----

static bool same_type(const struct type *a, const struct type *b) {
	if (a->mask != b->mask || a->levels != b->levels || a->entries_len != b->entries_len) {
		return false;
	}
	for (int i = 0; i < a->entries_len; i++) {
		if (a->entries[i].mods != b->entries[i].mods
				|| a->entries[i].level != b->entries[i].level) {
			return false;
		}
	}
	return true;
}

// which modifiers pick which level of the key in layout, as a type
static uint8_t type_of(struct xs_xkb *xkb, xkb_keycode_t keycode, xkb_layout_index_t layout) {
	struct type type = {.levels = 1};
	xkb_level_index_t levels = xkb_keymap_num_levels_for_key(xkb->keymap, keycode, layout);
	// 4 groups of 63 levels still count in a byte
	type.levels = (uint8_t)(levels < 1 ? 1 : levels > 63 ? 63 : levels);
	for (xkb_level_index_t level = 0; level < type.levels; level++) {
		xkb_mod_mask_t masks[MAX_ENTRIES];
		size_t n = xkb_keymap_key_get_mods_for_level(xkb->keymap, keycode, layout, level,
			masks, MAX_ENTRIES);
		for (size_t i = 0; i < n && type.entries_len < MAX_ENTRIES; i++) {
			uint8_t mods = (uint8_t)(masks[i] & 0xff);
			if (level == 0 && mods == 0) {
				continue; // no modifiers is level 1 anyway
			}
			type.entries[type.entries_len].mods = mods;
			type.entries[type.entries_len].level = (uint8_t)level;
			type.entries_len++;
			type.mask |= mods;
		}
	}
	for (int i = 0; i < xkb->types_len; i++) {
		if (same_type(&xkb->types[i], &type)) {
			return (uint8_t)i;
		}
	}
	if (xkb->types_len == MAX_TYPES) {
		return type.levels > 1 ? 1 : 0;
	}
	xkb->types[xkb->types_len] = type;
	return (uint8_t)xkb->types_len++;
}

// what a key does on its own: press it on a fresh state, see what changed,
// let go, see what stayed
static void action_of(struct xs_xkb *xkb, struct key *key, xkb_keycode_t keycode) {
	struct xkb_state *state = xkb_state_new(xkb->keymap);
	if (!state) {
		return;
	}
	xkb_state_update_key(state, keycode, XKB_KEY_DOWN);
	uint8_t held = (uint8_t)xkb_state_serialize_mods(state, XKB_STATE_MODS_DEPRESSED);
	int32_t held_group = (int32_t)xkb_state_serialize_layout(state, XKB_STATE_LAYOUT_DEPRESSED);
	xkb_state_update_key(state, keycode, XKB_KEY_UP);
	uint8_t latched = (uint8_t)xkb_state_serialize_mods(state, XKB_STATE_MODS_LATCHED);
	uint8_t locked = (uint8_t)xkb_state_serialize_mods(state, XKB_STATE_MODS_LOCKED);
	int32_t latched_group = (int32_t)xkb_state_serialize_layout(state,
		XKB_STATE_LAYOUT_LATCHED);
	int32_t locked_group = (int32_t)xkb_state_serialize_layout(state, XKB_STATE_LAYOUT_LOCKED);
	xkb_state_unref(state);
	if (locked) {
		key->action = SA_LOCK_MODS;
		key->action_mods = locked;
	} else if (latched) {
		key->action = SA_LATCH_MODS;
		key->action_mods = latched;
	} else if (held) {
		key->action = SA_SET_MODS;
		key->action_flags = SA_CLEAR_LOCKS;
		key->action_mods = held;
	} else if (locked_group) {
		key->action = SA_LOCK_GROUP;
		key->action_group = (int8_t)locked_group;
	} else if (latched_group) {
		key->action = SA_LATCH_GROUP;
		key->action_group = (int8_t)latched_group;
	} else if (held_group) {
		key->action = SA_SET_GROUP;
		key->action_group = (int8_t)held_group;
	}
	key->modmap = key->action_mods;
}

static void xkb_free(struct xs_xkb *xkb) {
	if (!xkb) {
		return;
	}
	for (int i = 0; i < 256; i++) {
		free(xkb->keys[i].syms);
	}
	xkb_keymap_unref(xkb->keymap);
	free(xkb->text);
	free(xkb);
}

static struct xs_xkb *xkb_from(struct flame_xserver *xserver, struct xkb_keymap *keymap,
		char *text) {
	struct xs_xkb *xkb = calloc(1, sizeof(*xkb));
	if (!xkb) {
		return NULL;
	}
	xkb->keymap = xkb_keymap_ref(keymap);
	xkb->text = text;
	xkb_keycode_t min = xkb_keymap_min_keycode(keymap), max = xkb_keymap_max_keycode(keymap);
	xkb->min = (uint8_t)(min < 8 ? 8 : min > 255 ? 255 : min);
	xkb->max = (uint8_t)(max > 255 ? 255 : max < xkb->min ? xkb->min : max);
	memcpy(xkb->types, canonical, sizeof(canonical));
	xkb->types_len = 4;
	xkb_layout_index_t layouts = xkb_keymap_num_layouts(keymap);
	xkb->groups = (int)(layouts < 1 ? 1 : layouts > MAX_GROUPS ? MAX_GROUPS : layouts);
	for (int g = 0; g < xkb->groups; g++) {
		xkb->group_names[g] = atom(xserver, xkb_keymap_layout_get_name(keymap, (uint32_t)g));
	}
	for (int k = xkb->min; k <= xkb->max; k++) {
		struct key *key = &xkb->keys[k];
		const char *name = xkb_keymap_key_get_name(keymap, (xkb_keycode_t)k);
		if (name) {
			strncpy(key->name, name, sizeof(key->name));
		}
		key->repeats = xkb_keymap_key_repeats(keymap, (xkb_keycode_t)k);
		xkb_layout_index_t groups = xkb_keymap_num_layouts_for_key(keymap, (xkb_keycode_t)k);
		key->groups = (uint8_t)(groups > MAX_GROUPS ? MAX_GROUPS : groups);
		for (int g = 0; g < key->groups; g++) {
			key->types[g] = type_of(xkb, (xkb_keycode_t)k, (uint32_t)g);
			uint8_t levels = xkb->types[key->types[g]].levels;
			key->width = levels > key->width ? levels : key->width;
		}
		if (key->groups) {
			key->syms = calloc((size_t)key->groups * key->width, sizeof(*key->syms));
			if (!key->syms) {
				xkb->text = NULL; // the caller still has it
				xkb_free(xkb);
				return NULL;
			}
		}
		for (int g = 0; g < key->groups; g++) {
			for (int l = 0; l < key->width; l++) {
				const xkb_keysym_t *syms;
				int n = xkb_keymap_key_get_syms_by_level(keymap, (xkb_keycode_t)k, (uint32_t)g,
					(uint32_t)l, &syms);
				key->syms[g * key->width + l] = n > 0 ? syms[0] : 0;
			}
		}
		action_of(xkb, key, (xkb_keycode_t)k);
	}
	xkb_mod_index_t mods = xkb_keymap_num_mods(keymap);
	for (xkb_mod_index_t m = 8; m < mods && m < 8 + NUM_VMODS; m++) {
		xkb->vmods |= (uint16_t)(1u << (m - 8));
		xkb->vmod_real[m - 8] = (uint8_t)xkb_keymap_mod_get_mask2(keymap, m);
		xkb->vmod_names[m - 8] = atom(xserver, xkb_keymap_mod_get_name(keymap, m));
	}
	xkb_led_index_t leds = xkb_keymap_num_leds(keymap);
	for (xkb_led_index_t i = 0; i < leds && i < MAX_LEDS; i++) {
		const char *name = xkb_keymap_led_get_name(keymap, i);
		if (!name) {
			continue;
		}
		xkb->leds |= 1u << i;
		xkb->led_names[i] = atom(xserver, name);
		if (!strcmp(name, "Caps Lock")) {
			xkb->led_mods[i] = LOCK;
		} else if (!strcmp(name, "Num Lock")) {
			xkb->led_mods[i] = MOD2;
		}
	}
	xkb->keycodes_name = atom(xserver, "evdev");
	xkb->symbols_name = atom(xserver, "flame");
	xkb->types_name = atom(xserver, "complete");
	xkb->compat_name = atom(xserver, "complete");
	return xkb;
}

// ---- telling clients ----

static void put_header(struct xs_event *event, uint8_t xkb_type, struct flame_xserver *xserver) {
	memset(event, 0, sizeof(*event));
	xs_event_put8(event, 0, XS_XKB_FIRST_EVENT);
	xs_event_put8(event, 1, xkb_type);
	xs_event_put32(event, 4, xs_now(xserver));
	xs_event_put8(event, 8, DEVICE_ID);
}

static uint8_t effective_mods(struct flame_xserver *xserver) {
	return xserver->kbd.base | xserver->kbd.latched | xserver->kbd.locked;
}

static void put_state(struct xs_event *event, struct flame_xserver *xserver, uint16_t changed) {
	uint8_t mods = effective_mods(xserver);
	put_header(event, STATE_NOTIFY, xserver);
	xs_event_put8(event, 9, mods);
	xs_event_put8(event, 10, xserver->kbd.base);
	xs_event_put8(event, 11, xserver->kbd.latched);
	xs_event_put8(event, 12, xserver->kbd.locked);
	xs_event_put8(event, 13, xserver->kbd.group);
	xs_event_put16(event, 14, (uint16_t)xserver->kbd.base_group);
	xs_event_put16(event, 16, (uint16_t)xserver->kbd.latched_group);
	xs_event_put8(event, 18, xserver->kbd.locked_group);
	xs_event_put8(event, 19, mods); // compat state
	xs_event_put8(event, 20, mods); // grab mods
	xs_event_put8(event, 21, mods);
	xs_event_put8(event, 22, mods); // lookup mods
	xs_event_put8(event, 23, mods);
	xs_event_put16(event, 24, xserver->state & 0x1f00);
	xs_event_put16(event, 26, changed);
}

void xserver_keyboard_state(struct flame_xserver *xserver, uint32_t depressed,
		uint32_t latched, uint32_t locked, uint32_t group) {
	struct xs_kbd_state old = xserver->kbd;
	struct xs_kbd_state *kbd = &xserver->kbd;
	kbd->base = (uint8_t)depressed;
	kbd->latched = (uint8_t)latched;
	kbd->locked = (uint8_t)locked;
	kbd->group = (uint8_t)(group & 3);
	kbd->locked_group = kbd->group; // flame keeps layouts as locked ones
	kbd->base_group = 0;
	kbd->latched_group = 0;
	uint16_t changed = 0;
	changed |= old.base != kbd->base ? MODIFIER_BASE : 0;
	changed |= old.latched != kbd->latched ? MODIFIER_LATCH : 0;
	changed |= old.locked != kbd->locked ? MODIFIER_LOCK : 0;
	changed |= old.locked_group != kbd->locked_group ? GROUP_LOCK : 0;
	changed |= old.group != kbd->group ? GROUP_STATE : 0;
	if ((old.base | old.latched | old.locked) != effective_mods(xserver)) {
		changed |= MODIFIER_STATE | COMPAT_STATE | GRAB_MODS | COMPAT_GRAB_MODS | LOOKUP_MODS
			| COMPAT_LOOKUP_MODS;
	}
	if (!changed) {
		return;
	}
	struct xs_event event;
	put_state(&event, xserver, changed);
	struct xs_client *client;
	wl_list_for_each(client, &xserver->clients, link) {
		if ((client->xkb_events & (1u << STATE_NOTIFY)) && (client->xkb_state_details & changed)) {
			xs_event_send(client, &event);
		}
	}
	if (old.locked != kbd->locked && xserver->xkb) {
		struct xs_event leds;
		put_header(&leds, INDICATOR_STATE_NOTIFY, xserver);
		uint32_t on = 0, was = 0;
		for (int i = 0; i < MAX_LEDS; i++) {
			uint8_t mods = xserver->xkb->led_mods[i];
			on |= mods && (kbd->locked & mods) ? 1u << i : 0;
			was |= mods && (old.locked & mods) ? 1u << i : 0;
		}
		xs_event_put32(&leds, 12, on);
		xs_event_put32(&leds, 16, on ^ was);
		wl_list_for_each(client, &xserver->clients, link) {
			if ((client->xkb_events & (1u << INDICATOR_STATE_NOTIFY)) && on != was) {
				xs_event_send(client, &leds);
			}
		}
	}
}

// a new keymap: XKB clients hear NewKeyboardNotify and MapNotify, everyone
// MappingNotify for the keyboard and the modifiers
static void keymap_changed(struct flame_xserver *xserver, uint8_t old_min, uint8_t old_max) {
	struct xs_xkb *xkb = xserver->xkb;
	struct xs_event nkn, map, core;
	put_header(&nkn, NEW_KEYBOARD_NOTIFY, xserver);
	xs_event_put8(&nkn, 9, DEVICE_ID);
	xs_event_put8(&nkn, 10, xkb->min);
	xs_event_put8(&nkn, 11, xkb->max);
	xs_event_put8(&nkn, 12, old_min);
	xs_event_put8(&nkn, 13, old_max);
	xs_event_put16(&nkn, 16, 1); // keycodes
	put_header(&map, MAP_NOTIFY, xserver);
	uint8_t keys = (uint8_t)(xkb->max - xkb->min + 1);
	xs_event_put16(&map, 10, ALL_MAP_PARTS);
	xs_event_put8(&map, 12, xkb->min);
	xs_event_put8(&map, 13, xkb->max);
	xs_event_put8(&map, 15, (uint8_t)xkb->types_len);
	for (int at = 16; at < 28; at += 2) {
		xs_event_put8(&map, (size_t)at, xkb->min);
		xs_event_put8(&map, (size_t)at + 1, keys);
	}
	xs_event_put16(&map, 28, xkb->vmods);
	struct xs_client *client;
	wl_list_for_each(client, &xserver->clients, link) {
		if (!client->setup_done) {
			continue;
		}
		if (client->xkb_events & (1u << NEW_KEYBOARD_NOTIFY)) {
			xs_event_send(client, &nkn);
		}
		if (client->xkb_events & (1u << MAP_NOTIFY)) {
			xs_event_send(client, &map);
		}
		for (int request = 0; request < 2; request++) { // modifiers, keyboard
			memset(&core, 0, sizeof(core));
			xs_event_put8(&core, 0, 34); // MappingNotify
			xs_event_put8(&core, 4, (uint8_t)request);
			xs_event_put8(&core, 5, request ? xkb->min : 0);
			xs_event_put8(&core, 6, request ? keys : 0);
			xs_event_send(client, &core);
		}
	}
}

void xserver_set_keymap_names(struct flame_xserver *xserver, const char *rules,
		const char *model, const char *layout, const char *variant, const char *options) {
	// rules, model, layout, variant, options: each ends in a nul
	const char *names[] = {rules, model, layout, variant, options};
	char buf[512];
	size_t len = 0;
	for (int i = 0; i < 5; i++) {
		const char *name = names[i] ? names[i] : "";
		size_t n = strlen(name);
		if (len + n + 1 > sizeof(buf)) {
			return;
		}
		memcpy(buf + len, name, n + 1);
		len += n + 1;
	}
	uint8_t error;
	xs_window_change_property(xserver, xserver->root, 0, atom(xserver, "_XKB_RULES_NAMES"),
		XS_ATOM_STRING, 8, (uint32_t)len, (const uint8_t *)buf, &error);
}

// what xkbcommon builds a keymap from without names: the environment
static const char *env_or(const char *name, const char *fallback) {
	const char *value = getenv(name);
	return value && *value ? value : fallback;
}

bool xs_xkb_init(struct flame_xserver *xserver) {
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = context
		? xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
	xkb_context_unref(context);
	if (!keymap) {
		return false;
	}
	xserver->repeat_delay = 600;
	xserver->repeat_rate = 25;
	xserver_set_keymap(xserver, keymap);
	xkb_keymap_unref(keymap);
	// flame builds its keymap the same way, from the same environment
	xserver_set_keymap_names(xserver, env_or("XKB_DEFAULT_RULES", "evdev"),
		env_or("XKB_DEFAULT_MODEL", "pc105"), env_or("XKB_DEFAULT_LAYOUT", "us"),
		env_or("XKB_DEFAULT_VARIANT", ""), env_or("XKB_DEFAULT_OPTIONS", ""));
	return xserver->xkb != NULL;
}

void xs_xkb_finish(struct flame_xserver *xserver) {
	xkb_free(xserver->xkb);
	xserver->xkb = NULL;
}

void xserver_set_keymap(struct flame_xserver *xserver, struct xkb_keymap *keymap) {
	if (!keymap || (xserver->xkb && xserver->xkb->keymap == keymap)) {
		return;
	}
	char *text = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
	if (!text) {
		return;
	}
	struct xs_xkb *old = xserver->xkb;
	if (old && !strcmp(old->text, text)) {
		// the same keymap, a keyboard of its own: nothing to tell anyone
		xkb_keymap_unref(old->keymap);
		old->keymap = xkb_keymap_ref(keymap);
		free(text);
		return;
	}
	struct xs_xkb *xkb = xkb_from(xserver, keymap, text);
	if (!xkb) {
		free(text);
		return;
	}
	xserver->xkb = xkb;
	if (old) {
		keymap_changed(xserver, old->min, old->max);
		xkb_free(old);
	}
}

void xserver_set_repeat(struct flame_xserver *xserver, int rate, int delay) {
	xserver->repeat_rate = rate;
	xserver->repeat_delay = delay;
}

// ---- the core keyboard, from the same keymap ----

int xs_xkb_core_width(struct flame_xserver *xserver) {
	struct xs_xkb *xkb = xserver->xkb;
	int width = 2;
	for (int k = xkb->min; k <= xkb->max; k++) {
		width = xkb->keys[k].width > width ? xkb->keys[k].width : width;
	}
	// like Xorg: group 1's first two levels, group 2's, then the rest of
	// group 1's and group 2's. no more than a core client can be handed
	int core = width > 2 ? 2 + 2 + (width - 2) * 2 : 4;
	return core < XS_XKB_CORE_MAX ? core : XS_XKB_CORE_MAX;
}

void xs_xkb_core_syms(struct flame_xserver *xserver, uint8_t keycode, uint32_t *out,
		int width) {
	memset(out, 0, (size_t)width * sizeof(*out));
	struct xs_xkb *xkb = xserver->xkb;
	if (keycode < xkb->min || keycode > xkb->max || !xkb->keys[keycode].groups) {
		return;
	}
	struct key *key = &xkb->keys[keycode];
	int g2 = key->groups > 1 ? 1 : 0;
	int at = 0;
	for (int g = 0; g < 2; g++) {
		int group = g ? g2 : 0;
		for (int l = 0; l < 2 && at < width; l++) {
			out[at++] = l < key->width ? key->syms[group * key->width + l] : 0;
		}
	}
	for (int g = 0; g < 2; g++) {
		int group = g ? g2 : 0;
		for (int l = 2; l < key->width && at < width; l++) {
			out[at++] = key->syms[group * key->width + l];
		}
	}
	// one level keys say it twice, the way core clients expect a letter
	if (out[1] == 0 && out[0] != 0) {
		out[1] = out[0];
	}
	if (out[3] == 0 && out[2] == 0) {
		out[2] = out[0];
		out[3] = out[1];
	}
}

uint8_t xs_xkb_modmap(struct flame_xserver *xserver, uint8_t keycode) {
	struct xs_xkb *xkb = xserver->xkb;
	return keycode >= xkb->min && keycode <= xkb->max ? xkb->keys[keycode].modmap : 0;
}

uint8_t xs_xkb_range(struct flame_xserver *xserver, uint8_t *max) {
	*max = xserver->xkb->max;
	return xserver->xkb->min;
}

// ---- requests ----

static void use_extension(struct xs_client *client) {
	client->xkb = true;
	struct xout out;
	xs_reply_begin(client, &out, 1); // supported
	xout16(&out, MAJOR);
	xout16(&out, MINOR);
	xs_reply_send(client, &out);
}

// the bytes each event type's details take in SelectEvents, in order
static const uint8_t detail_sizes[12] = {4, 0, 4, 8, 8, 8, 4, 2, 2, 2, 4, 4};

static void select_events(struct xs_client *client, const struct xin *in) {
	uint16_t affect = xin16(in, 6), clear = xin16(in, 8), all = xin16(in, 10);
	// MapNotify's details are in the fixed part, the rest follow in bit order
	size_t at = 16;
	for (int bit = 0; bit < 12; bit++) {
		uint16_t b = (uint16_t)(1u << bit);
		if (!(affect & b)) {
			continue;
		}
		if (clear & b) {
			client->xkb_events &= (uint16_t)~b;
			if (bit == STATE_NOTIFY) {
				client->xkb_state_details = 0;
			}
			continue;
		}
		if (all & b) {
			client->xkb_events |= b;
			if (bit == STATE_NOTIFY) {
				client->xkb_state_details = 0xffff;
			}
			continue;
		}
		if (bit == MAP_NOTIFY) {
			uint16_t affect_map = xin16(in, 12), map = xin16(in, 14);
			client->xkb_events = (affect_map & map) ? client->xkb_events | b
				: client->xkb_events & (uint16_t)~b;
			continue;
		}
		uint32_t affect_details, details;
		if (detail_sizes[bit] == 8) {
			affect_details = xin32(in, at);
			details = xin32(in, at + 4);
		} else if (detail_sizes[bit] == 4) {
			affect_details = xin16(in, at);
			details = xin16(in, at + 2);
		} else {
			affect_details = xin8(in, at);
			details = xin8(in, at + 1);
		}
		at += detail_sizes[bit];
		if (bit == STATE_NOTIFY) {
			client->xkb_state_details = (uint16_t)((client->xkb_state_details
				& ~affect_details) | (details & affect_details));
			details = client->xkb_state_details;
		}
		client->xkb_events = (affect_details & details) || (bit == STATE_NOTIFY && details)
			? client->xkb_events | b : client->xkb_events & (uint16_t)~b;
	}
}

static void get_state(struct xs_client *client) {
	struct flame_xserver *xserver = client->xserver;
	struct xs_kbd_state *kbd = &xserver->kbd;
	uint8_t mods = effective_mods(xserver);
	struct xout out;
	xs_reply_begin(client, &out, DEVICE_ID);
	xout8(&out, mods);
	xout8(&out, kbd->base);
	xout8(&out, kbd->latched);
	xout8(&out, kbd->locked);
	xout8(&out, kbd->group);
	xout8(&out, kbd->locked_group);
	xout16(&out, (uint16_t)kbd->base_group);
	xout16(&out, (uint16_t)kbd->latched_group);
	xout8(&out, mods); // compat state
	xout8(&out, mods); // grab mods
	xout8(&out, mods);
	xout8(&out, mods); // lookup mods
	xout8(&out, mods);
	xout8(&out, 0);
	xout16(&out, xserver->state & 0x1f00);
	xs_reply_send(client, &out);
}

static void get_controls(struct xs_client *client) {
	struct flame_xserver *xserver = client->xserver;
	struct xs_xkb *xkb = xserver->xkb;
	struct xout out;
	xs_reply_begin(client, &out, DEVICE_ID);
	xout8(&out, 1); // mouse keys' default button
	xout8(&out, (uint8_t)xkb->groups);
	xout8(&out, 0); // groups wrap
	xout_zero(&out, 5); // internal and ignore-lock mods
	xout_zero(&out, 4); // and their vmods
	xout16(&out, (uint16_t)xserver->repeat_delay);
	xout16(&out, (uint16_t)(xserver->repeat_rate > 0 ? 1000 / xserver->repeat_rate : 0));
	xout_zero(&out, 2 * 7); // slow keys to the mouse keys curve
	xout_zero(&out, 2 * 4); // access x
	xout_zero(&out, 2);
	xout32(&out, 0); // access x timeout mask
	xout32(&out, 0);
	xout32(&out, xserver->repeat_rate > 0 ? REPEAT_KEYS_CONTROL : 0); // enabled controls
	uint8_t repeat[32] = {0};
	for (int k = xkb->min; k <= xkb->max; k++) {
		repeat[k / 8] |= xkb->keys[k].repeats ? (uint8_t)(1u << (k % 8)) : 0;
	}
	xout_bytes(&out, repeat, sizeof(repeat));
	xs_reply_send(client, &out);
}

// a part's range: the whole keyboard when it's in full, what was asked for
// when it's in partial, else nothing
static bool part_range(struct xs_xkb *xkb, uint16_t full, uint16_t partial, uint16_t part,
		uint8_t first, uint8_t count, uint8_t *out_first, uint8_t *out_count) {
	*out_first = 0;
	*out_count = 0;
	if (full & part) {
		*out_first = xkb->min;
		*out_count = (uint8_t)(xkb->max - xkb->min + 1);
		return true;
	}
	if (partial & part) {
		int last = first + count - 1;
		int lo = first < xkb->min ? xkb->min : first;
		int hi = last > xkb->max ? xkb->max : last;
		if (count && hi >= lo) {
			*out_first = (uint8_t)lo;
			*out_count = (uint8_t)(hi - lo + 1);
		} else {
			*out_first = xkb->min;
		}
		return true;
	}
	return false;
}

static void put_action(struct xout *out, const struct key *key) {
	uint8_t action[8] = {key->action, key->action_flags};
	if (key->action >= SA_SET_GROUP) {
		action[2] = (uint8_t)key->action_group;
	} else {
		action[2] = key->action_mods; // mask
		action[3] = key->action_mods; // real mods
	}
	xout_bytes(out, action, sizeof(action));
}

static void get_map(struct xs_client *client, const struct xin *in) {
	struct xs_xkb *xkb = client->xserver->xkb;
	uint16_t full = xin16(in, 6), partial = xin16(in, 8);
	uint8_t first_type = xin8(in, 10), n_types = xin8(in, 11);
	uint16_t want_vmods = xin16(in, 18);
	uint16_t present = 0;

	int type_lo = 0, type_n = 0;
	if (full & KEY_TYPES) {
		type_n = xkb->types_len;
		present |= KEY_TYPES;
	} else if (partial & KEY_TYPES) {
		type_lo = first_type < xkb->types_len ? first_type : xkb->types_len;
		type_n = first_type + n_types <= xkb->types_len ? n_types : xkb->types_len - type_lo;
		present |= KEY_TYPES;
	}
	uint8_t syms_first, syms_n, acts_first, acts_n, beh_first, beh_n, exp_first, exp_n;
	uint8_t mm_first, mm_n, vmm_first, vmm_n;
	present |= part_range(xkb, full, partial, KEY_SYMS, xin8(in, 12), xin8(in, 13),
		&syms_first, &syms_n) ? KEY_SYMS : 0;
	present |= part_range(xkb, full, partial, KEY_ACTIONS, xin8(in, 14), xin8(in, 15),
		&acts_first, &acts_n) ? KEY_ACTIONS : 0;
	present |= part_range(xkb, full, partial, KEY_BEHAVIORS, xin8(in, 16), xin8(in, 17),
		&beh_first, &beh_n) ? KEY_BEHAVIORS : 0;
	present |= part_range(xkb, full, partial, EXPLICIT_COMPONENTS, xin8(in, 20), xin8(in, 21),
		&exp_first, &exp_n) ? EXPLICIT_COMPONENTS : 0;
	present |= part_range(xkb, full, partial, MODIFIER_MAP, xin8(in, 22), xin8(in, 23),
		&mm_first, &mm_n) ? MODIFIER_MAP : 0;
	present |= part_range(xkb, full, partial, VIRTUAL_MOD_MAP, xin8(in, 24), xin8(in, 25),
		&vmm_first, &vmm_n) ? VIRTUAL_MOD_MAP : 0;
	uint16_t vmods = 0;
	if (full & VIRTUAL_MODS) {
		vmods = 0xffff;
		present |= VIRTUAL_MODS;
	} else if (partial & VIRTUAL_MODS) {
		vmods = want_vmods;
		present |= VIRTUAL_MODS;
	}

	uint16_t total_syms = 0, total_actions = 0;
	for (int k = syms_first; k < syms_first + syms_n; k++) {
		total_syms = (uint16_t)(total_syms + xkb->keys[k].groups * xkb->keys[k].width);
	}
	for (int k = acts_first; k < acts_first + acts_n; k++) {
		const struct key *key = &xkb->keys[k];
		total_actions = (uint16_t)(total_actions + (key->action ? key->groups * key->width : 0));
	}
	uint8_t total_modmap = 0;
	for (int k = mm_first; k < mm_first + mm_n; k++) {
		total_modmap = (uint8_t)(total_modmap + (xkb->keys[k].modmap != 0));
	}

	struct xout out;
	xs_reply_begin(client, &out, DEVICE_ID);
	xout_zero(&out, 2);
	xout8(&out, xkb->min);
	xout8(&out, xkb->max);
	xout16(&out, present);
	xout8(&out, (uint8_t)type_lo);
	xout8(&out, (uint8_t)type_n);
	xout8(&out, (uint8_t)xkb->types_len);
	xout8(&out, syms_first);
	xout16(&out, total_syms);
	xout8(&out, syms_n);
	xout8(&out, acts_first);
	xout16(&out, total_actions);
	xout8(&out, acts_n);
	xout8(&out, beh_first);
	xout8(&out, beh_n);
	xout8(&out, 0); // total behaviors
	xout8(&out, exp_first);
	xout8(&out, exp_n);
	xout8(&out, 0); // total explicit
	xout8(&out, mm_first);
	xout8(&out, mm_n);
	xout8(&out, total_modmap);
	xout8(&out, vmm_first);
	xout8(&out, vmm_n);
	xout8(&out, 0); // total vmodmap
	xout8(&out, 0);
	xout16(&out, vmods);

	for (int t = type_lo; t < type_lo + type_n; t++) {
		const struct type *type = &xkb->types[t];
		xout8(&out, type->mask);
		xout8(&out, type->mask); // real mods
		xout16(&out, 0); // vmods
		xout8(&out, type->levels);
		xout8(&out, type->entries_len);
		xout8(&out, 0); // no preserve
		xout8(&out, 0);
		for (int e = 0; e < type->entries_len; e++) {
			xout8(&out, 1); // active
			xout8(&out, type->entries[e].mods);
			xout8(&out, type->entries[e].level);
			xout8(&out, type->entries[e].mods);
			xout16(&out, 0);
			xout16(&out, 0);
		}
	}
	for (int k = syms_first; k < syms_first + syms_n; k++) {
		const struct key *key = &xkb->keys[k];
		for (int g = 0; g < MAX_GROUPS; g++) {
			xout8(&out, g < key->groups ? key->types[g] : 0);
		}
		xout8(&out, key->groups); // group info: wrap
		xout8(&out, key->width);
		xout16(&out, (uint16_t)(key->groups * key->width));
		for (int i = 0; i < key->groups * key->width; i++) {
			xout32(&out, key->syms[i]);
		}
	}
	if (present & KEY_ACTIONS) {
		for (int k = acts_first; k < acts_first + acts_n; k++) {
			const struct key *key = &xkb->keys[k];
			xout8(&out, (uint8_t)(key->action ? key->groups * key->width : 0));
		}
		xout_pad(&out);
		for (int k = acts_first; k < acts_first + acts_n; k++) {
			const struct key *key = &xkb->keys[k];
			for (int i = 0; key->action && i < key->groups * key->width; i++) {
				put_action(&out, key);
			}
		}
	}
	if (present & VIRTUAL_MODS) {
		for (int v = 0; v < NUM_VMODS; v++) {
			if (vmods & (1u << v)) {
				xout8(&out, xkb->vmod_real[v]);
			}
		}
		xout_pad(&out);
	}
	if (present & MODIFIER_MAP) {
		for (int k = mm_first; k < mm_first + mm_n; k++) {
			if (xkb->keys[k].modmap) {
				xout8(&out, (uint8_t)k);
				xout8(&out, xkb->keys[k].modmap);
			}
		}
		xout_pad(&out);
	}
	xs_reply_send(client, &out);
}

static void get_compat_map(struct xs_client *client, const struct xin *in) {
	uint8_t groups = xin8(in, 6);
	struct xout out;
	xs_reply_begin(client, &out, DEVICE_ID);
	xout8(&out, groups);
	xout8(&out, 0);
	xout16(&out, 0); // first interpretation
	xout16(&out, 0); // none: the actions are on the keys already
	xout16(&out, 0);
	xout_zero(&out, 16);
	for (int g = 0; g < MAX_GROUPS; g++) {
		if (groups & (1u << g)) {
			xout32(&out, 0); // no modifiers for any group
		}
	}
	xs_reply_send(client, &out);
}

static void put_indicator(struct xout *out, uint8_t mods) {
	xout8(out, 0); // flags
	xout8(out, 0); // which groups
	xout8(out, 0);
	xout8(out, mods ? 4 : 0); // which mods: locked
	xout8(out, mods);
	xout8(out, mods);
	xout16(out, 0);
	xout32(out, 0); // controls
}

static uint32_t leds_on(struct flame_xserver *xserver) {
	struct xs_xkb *xkb = xserver->xkb;
	uint32_t on = 0;
	for (int i = 0; i < MAX_LEDS; i++) {
		if (xkb->led_mods[i] && (xserver->kbd.locked & xkb->led_mods[i])) {
			on |= 1u << i;
		}
	}
	return on;
}

static void get_indicator_map(struct xs_client *client, const struct xin *in) {
	struct xs_xkb *xkb = client->xserver->xkb;
	uint32_t which = xin32(in, 8);
	struct xout out;
	xs_reply_begin(client, &out, DEVICE_ID);
	xout32(&out, which);
	xout32(&out, xkb->leds);
	xout8(&out, (uint8_t)__builtin_popcount(xkb->leds));
	xout_zero(&out, 15);
	for (int i = 0; i < MAX_LEDS; i++) {
		if (which & (1u << i)) {
			put_indicator(&out, xkb->led_mods[i]);
		}
	}
	xs_reply_send(client, &out);
}

static void get_named_indicator(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	struct xs_xkb *xkb = xserver->xkb;
	uint32_t name = xin32(in, 12);
	int found = -1;
	for (int i = 0; i < MAX_LEDS; i++) {
		if ((xkb->leds & (1u << i)) && xkb->led_names[i] == name) {
			found = i;
		}
	}
	uint8_t mods = found >= 0 ? xkb->led_mods[found] : 0;
	struct xout out;
	xs_reply_begin(client, &out, DEVICE_ID);
	xout32(&out, name);
	xout8(&out, found >= 0);
	xout8(&out, found >= 0 && (leds_on(xserver) >> found & 1));
	xout8(&out, found >= 0); // real
	xout8(&out, (uint8_t)(found >= 0 ? found : 0));
	put_indicator(&out, mods);
	xout8(&out, 1); // supported
	xs_reply_send(client, &out);
}

static void get_names(struct xs_client *client, const struct xin *in) {
	struct xs_xkb *xkb = client->xserver->xkb;
	// everything but geometry, physical symbols and radio groups
	uint32_t which = xin32(in, 8) & (1 | 4 | 16 | 32 | 64 | 128 | 256 | 512 | 1024 | 2048
		| 4096);
	uint16_t vmods = 0;
	for (int v = 0; v < NUM_VMODS; v++) {
		vmods |= xkb->vmod_names[v] ? (uint16_t)(1u << v) : 0;
	}
	uint8_t group_names = 0;
	for (int g = 0; g < xkb->groups; g++) {
		group_names |= xkb->group_names[g] ? (uint8_t)(1u << g) : 0;
	}
	uint16_t kt_levels = 0;
	for (int t = 0; t < xkb->types_len; t++) {
		kt_levels = (uint16_t)(kt_levels + xkb->types[t].levels);
	}
	struct xout out;
	xs_reply_begin(client, &out, DEVICE_ID);
	xout32(&out, which);
	xout8(&out, xkb->min);
	xout8(&out, xkb->max);
	xout8(&out, (uint8_t)xkb->types_len);
	xout8(&out, group_names);
	xout16(&out, vmods);
	xout8(&out, xkb->min); // first key
	xout8(&out, (uint8_t)(xkb->max - xkb->min + 1));
	xout32(&out, xkb->leds);
	xout8(&out, 0); // radio groups
	xout8(&out, 0); // key aliases
	xout16(&out, kt_levels);
	xout_zero(&out, 4);
	if (which & 1) {
		xout32(&out, xkb->keycodes_name);
	}
	if (which & 4) {
		xout32(&out, xkb->symbols_name);
	}
	if (which & 16) {
		xout32(&out, xkb->types_name);
	}
	if (which & 32) {
		xout32(&out, xkb->compat_name);
	}
	if (which & 64) {
		for (int t = 0; t < xkb->types_len; t++) {
			char name[16];
			snprintf(name, sizeof(name), "FLAME_%d", t);
			xout32(&out, atom(client->xserver, xkb->types[t].name ? xkb->types[t].name : name));
		}
	}
	if (which & 128) {
		for (int t = 0; t < xkb->types_len; t++) {
			xout8(&out, xkb->types[t].levels);
		}
		xout_pad(&out);
		for (int t = 0; t < xkb->types_len; t++) {
			for (int l = 0; l < xkb->types[t].levels; l++) {
				char name[16];
				snprintf(name, sizeof(name), "Level%d", l + 1);
				xout32(&out, atom(client->xserver, name));
			}
		}
	}
	if (which & 256) {
		for (int i = 0; i < MAX_LEDS; i++) {
			if (xkb->leds & (1u << i)) {
				xout32(&out, xkb->led_names[i]);
			}
		}
	}
	if (which & 2048) {
		for (int v = 0; v < NUM_VMODS; v++) {
			if (vmods & (1u << v)) {
				xout32(&out, xkb->vmod_names[v]);
			}
		}
	}
	if (which & 4096) {
		for (int g = 0; g < xkb->groups; g++) {
			if (group_names & (1u << g)) {
				xout32(&out, xkb->group_names[g]);
			}
		}
	}
	if (which & 512) {
		for (int k = xkb->min; k <= xkb->max; k++) {
			xout_bytes(&out, xkb->keys[k].name, 4);
		}
	}
	xs_reply_send(client, &out);
}

static void per_client_flags(struct xs_client *client, const struct xin *in) {
	uint32_t change = xin32(in, 8), value = xin32(in, 12);
	if (change & DETECTABLE_AUTO_REPEAT) {
		client->detectable_repeat = value & DETECTABLE_AUTO_REPEAT;
	}
	struct xout out;
	xs_reply_begin(client, &out, DEVICE_ID);
	xout32(&out, ALL_CLIENT_FLAGS);
	xout32(&out, client->detectable_repeat ? DETECTABLE_AUTO_REPEAT : 0);
	xout32(&out, 0);
	xout32(&out, 0);
	xs_reply_send(client, &out);
}

static void get_device_info(struct xs_client *client) {
	static const char name[] = "Virtual core keyboard";
	struct xout out;
	xs_reply_begin(client, &out, DEVICE_ID);
	xout16(&out, 0); // present
	xout16(&out, 0); // supported
	xout16(&out, 0); // unsupported
	xout16(&out, 0); // led feedbacks
	xout_zero(&out, 6); // buttons, own state
	xout16(&out, 0); // default keyboard feedback
	xout16(&out, 0); // default led feedback
	xout16(&out, 0);
	xout32(&out, atom(client->xserver, "KEYBOARD"));
	xout16(&out, sizeof(name) - 1);
	xout_bytes(&out, name, sizeof(name) - 1);
	xout_pad(&out);
	xs_reply_send(client, &out);
}

void xs_xkb_request(struct xs_client *client, const struct xin *in) {
	struct flame_xserver *xserver = client->xserver;
	struct xout out;
	uint8_t minor = xin8(in, 1);
	if (minor != 0 && minor != 101) {
		uint16_t device = xin16(in, 4);
		if (device != 0x100 && device != DEVICE_ID) { // XkbUseCoreKbd
			xs_error(client, BAD_KEYBOARD, device, XS_XKB_OPCODE, minor);
			return;
		}
	}
	switch (minor) {
	case 0:
		use_extension(client);
		break;
	case 1:
		select_events(client, in);
		break;
	case 4:
		get_state(client);
		break;
	case 6:
		get_controls(client);
		break;
	case 8:
		get_map(client, in);
		break;
	case 10:
		get_compat_map(client, in);
		break;
	case 12: // GetIndicatorState
		xs_reply_begin(client, &out, DEVICE_ID);
		xout32(&out, leds_on(xserver));
		xs_reply_send(client, &out);
		break;
	case 13:
		get_indicator_map(client, in);
		break;
	case 15:
		get_named_indicator(client, in);
		break;
	case 17:
		get_names(client, in);
		break;
	case 19: // GetGeometry: there's none
		xs_reply_begin(client, &out, DEVICE_ID);
		xout32(&out, xin32(in, 8));
		xout8(&out, 0); // found
		xout_zero(&out, 1 + 16 + 2);
		xout16(&out, 0); // label font, an empty string
		xout_zero(&out, 2);
		xs_reply_send(client, &out);
		break;
	case 21:
		per_client_flags(client, in);
		break;
	case 22: // ListComponents: nothing to pick from
		xs_reply_begin(client, &out, DEVICE_ID);
		xout_zero(&out, 7 * 2);
		xs_reply_send(client, &out);
		break;
	case 23: // GetKbdByName: flame picks keymaps
		xs_error(client, XS_ERROR_ACCESS, 0, XS_XKB_OPCODE, minor);
		break;
	case 24:
		get_device_info(client);
		break;
	case 101: // SetDebuggingFlags
		xs_reply_begin(client, &out, 0);
		xout_zero(&out, 16);
		xs_reply_send(client, &out);
		break;
	// accepted, nothing changes: Bell, LatchLockState, SetControls, SetMap,
	// SetCompatMap, SetIndicatorMap, SetNamedIndicator, SetNames,
	// SetGeometry, SetDeviceInfo
	case 3: case 5: case 7: case 9: case 11: case 14: case 16: case 18: case 20: case 25:
		break;
	default:
		xs_error(client, XS_ERROR_REQUEST, 0, XS_XKB_OPCODE, minor);
		break;
	}
}
