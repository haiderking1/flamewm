#include "input/fake.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_keyboard.h>
#include <xkbcommon/xkbcommon.h>

#include "core/server.h"
#include "input/keyboard.h"

// a keyboard that isn't there, for tests (FLAME_TEST_INPUT=1): it joins like
// a plugged in one and its keys go the way real ones do, keybinds and all

struct flame_fake_keyboard {
	struct wlr_keyboard wlr;
};

static const struct wlr_keyboard_impl fake_impl = {
	.name = "flame-test-keyboard",
};

bool fake_keyboard_create(struct flame_input *input) {
	struct flame_fake_keyboard *keyboard = calloc(1, sizeof(*keyboard));
	if (!keyboard) {
		return false;
	}
	wlr_keyboard_init(&keyboard->wlr, &fake_impl, "flame-test-keyboard");
	keyboard_create(input, &keyboard->wlr.base);
	input->fake = keyboard;
	return true;
}

void fake_keyboard_destroy(struct flame_input *input) {
	if (input->fake) {
		wlr_keyboard_finish(&input->fake->wlr);
		free(input->fake);
		input->fake = NULL;
	}
}

static uint32_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

struct find {
	xkb_keysym_t sym;
	xkb_keycode_t found;
};

static void find_key(struct xkb_keymap *keymap, xkb_keycode_t key, void *data) {
	struct find *find = data;
	const xkb_keysym_t *syms;
	if (!find->found && xkb_keymap_key_get_syms_by_level(keymap, key, 0, 0, &syms) > 0
			&& syms[0] == find->sym) {
		find->found = key;
	}
}

bool fake_key(struct flame_input *input, const char *name, bool pressed) {
	if (!input->fake || !input->fake->wlr.keymap) {
		return false;
	}
	static const struct {
		const char *alias, *name;
	} aliases[] = {
		{"super", "Super_L"}, {"shift", "Shift_L"}, {"ctrl", "Control_L"}, {"alt", "Alt_L"},
		{"enter", "Return"},
	};
	for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
		if (!strcasecmp(name, aliases[i].alias)) {
			name = aliases[i].name;
		}
	}
	struct find find = {xkb_keysym_from_name(name, XKB_KEYSYM_NO_FLAGS), 0};
	if (find.sym == XKB_KEY_NoSymbol) {
		find.sym = xkb_keysym_from_name(name, XKB_KEYSYM_CASE_INSENSITIVE);
	}
	xkb_keymap_key_for_each(input->fake->wlr.keymap, find_key, &find);
	if (!find.found) {
		return false;
	}
	struct wlr_keyboard_key_event event = {
		.time_msec = now_ms(),
		.keycode = find.found - 8,
		.update_state = true,
		.state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED,
	};
	wlr_keyboard_notify_key(&input->fake->wlr, &event);
	return true;
}
