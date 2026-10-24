#include "ipc/commands.h"

#include <errno.h>
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>

#include "bindings/action.h"
#include "config/settings.h"
#include "core/server.h"
#include "input/fake.h"
#include "ipc/ipc.h"
#include "ipc/state.h"
#include "util/macros.h"
#include "util/strbuf.h"

enum command_arg {
	ARG_NONE,
	ARG_WORKSPACE, // 1..FLAME_WORKSPACE_COUNT, handed to the action as an index
	ARG_DIRECTION, // next / prev
};

// the same actions keybinds run, so a bar click and a key press can't drift apart
static const struct command {
	const char *name;
	flame_action_fn action;
	enum command_arg arg;
} commands[] = {
	{ "workspace", action_workspace, ARG_WORKSPACE },
	{ "movetoworkspace", action_move_to_workspace, ARG_WORKSPACE },
	{ "focus", action_focus, ARG_DIRECTION },
	{ "fullscreen", action_fullscreen, ARG_NONE },
	{ "togglefloating", action_toggle_floating, ARG_NONE },
	{ "reload", action_reload, ARG_NONE },
	{ "close", action_close, ARG_NONE },
	{ "quit", action_quit, ARG_NONE },
};

static void reply_ok(struct flame_ipc_client *client) {
	static const char ok[] = "{\"ok\":true}";
	ipc_client_send(client, ok, sizeof(ok) - 1);
}

static void reply_error(struct flame_ipc_client *client, const char *error) {
	struct strbuf out;
	strbuf_init(&out);
	strbuf_printf(&out, "{\"ok\":false,\"error\":");
	strbuf_json_string(&out, error);
	strbuf_printf(&out, "}");
	if (!out.failed) {
		ipc_client_send(client, out.data, out.len);
	}
	strbuf_finish(&out);
}

static void send_state(struct flame_ipc_client *client) {
	struct strbuf out;
	strbuf_init(&out);
	ipc_state_write(&client->ipc->server->desktop, &out);
	if (!out.failed) {
		ipc_client_send(client, out.data, out.len);
	}
	strbuf_finish(&out);
}

static bool parse_arg(enum command_arg kind, const char *word, union flame_arg *arg,
		const char **error) {
	if (kind == ARG_NONE) {
		if (word) {
			*error = "this command takes no argument";
			return false;
		}
		return true;
	}
	if (!word) {
		*error = kind == ARG_WORKSPACE ? "which workspace? 1-10" : "next or prev?";
		return false;
	}
	if (kind == ARG_DIRECTION) {
		if (!strcmp(word, "next") || !strcmp(word, "prev")) {
			arg->i = !strcmp(word, "next") ? +1 : -1;
			return true;
		}
		*error = "next or prev?";
		return false;
	}
	char *end;
	errno = 0;
	long n = strtol(word, &end, 10);
	if (errno || *end || n < 1 || n > FLAME_WORKSPACE_COUNT) {
		*error = "workspaces are 1-10";
		return false;
	}
	arg->i = (int)n - 1;
	return true;
}

// pointer move X Y, pointer nudge DX DY (a mouse moving by that much),
// pointer button left|middle|right press|release, pointer scroll DELTA (15
// a wheel notch, down positive): tests
// drive flame with these, and only when it was started for that
static void pointer_command(struct flame_ipc_client *client, char **save) {
	if (!getenv("FLAME_TEST_INPUT")) {
		reply_error(client, "start flame with FLAME_TEST_INPUT=1 for this");
		return;
	}
	struct flame_cursor *cursor = &client->ipc->server->input.cursor;
	const char *what = strtok_r(NULL, " \t\r", save);
	const char *a = strtok_r(NULL, " \t\r", save);
	const char *b = strtok_r(NULL, " \t\r", save);
	if (what && a && !b && !strcmp(what, "scroll")) {
		reply_ok(client);
		cursor_test_scroll(cursor, strtod(a, NULL));
		return;
	}
	if (what && a && b && !strcmp(what, "move")) {
		reply_ok(client);
		cursor_test_move(cursor, strtod(a, NULL), strtod(b, NULL));
		return;
	}
	if (what && a && b && !strcmp(what, "nudge")) {
		reply_ok(client);
		cursor_test_nudge(cursor, strtod(a, NULL), strtod(b, NULL));
		return;
	}
	if (what && a && b && !strcmp(what, "button")) {
		uint32_t button = !strcmp(a, "left") ? BTN_LEFT : !strcmp(a, "right") ? BTN_RIGHT
			: !strcmp(a, "middle") ? BTN_MIDDLE : 0;
		if (button) {
			reply_ok(client);
			cursor_test_button(cursor, button, !strcmp(b, "press"));
			return;
		}
	}
	reply_error(client, "pointer move X Y, pointer nudge DX DY, "
		"pointer button left|middle|right press|release, or pointer scroll DELTA");
}

void ipc_command(struct flame_ipc_client *client, char *line) {
	char *save = NULL;
	const char *name = strtok_r(line, " \t\r", &save);
	if (!name) {
		return; // empty line, nothing to answer
	}
	if (!strcmp(name, "pointer")) {
		pointer_command(client, &save);
		return;
	}
	if (!strcmp(name, "settings")) {
		struct strbuf out;
		strbuf_init(&out);
		settings_write(client->ipc->server, &out);
		if (!out.failed) {
			ipc_client_send(client, out.data, out.len);
		}
		strbuf_finish(&out);
		return;
	}
	if (!strcmp(name, "set") || !strcmp(name, "unset")) {
		// set KEY VALUE (the rest of the line, spaces and all), unset KEY
		const char *key = strtok_r(NULL, " \t\r", &save);
		const char *value = save;
		while (value && (*value == ' ' || *value == '\t')) {
			value++;
		}
		const char *error = NULL;
		bool set = !strcmp(name, "set");
		if (!key || (set && (!value || !*value))) {
			reply_error(client, set ? "set KEY VALUE" : "unset KEY");
		} else if (set ? settings_set(client->ipc->server, key, value, &error)
				: settings_unset(client->ipc->server, key, &error)) {
			reply_ok(client);
		} else {
			reply_error(client, error);
		}
		return;
	}
	if (!strcmp(name, "share-pick")) {
		// the answer comes when the user picks, raw for the portal's chooser:
		// "Monitor: NAME" or "Window: ID", an error when cancelled. "screen"
		// and "window" after it: what the app asked for, both when neither
		unsigned kinds = 0;
		const char *kind;
		while ((kind = strtok_r(NULL, " \t\r", &save))) {
			kinds |= !strcmp(kind, "screen") ? PICKER_SCREENS
				: !strcmp(kind, "window") ? PICKER_WINDOWS : 0;
		}
		picker_open(&client->ipc->server->picker, client, kinds);
		return;
	}
	if (!strcmp(name, "test")) {
		// a vt switch where the driver drops the screens: they go and the
		// keyboards with them, then all of it comes back
		const char *what = strtok_r(NULL, " \t\r", &save);
		struct flame_server *server = client->ipc->server;
		if (!getenv("FLAME_TEST_INPUT")) {
			reply_error(client, "start flame with FLAME_TEST_INPUT=1 for this");
		} else if (what && !strcmp(what, "unplug")) {
			reply_ok(client);
			fake_keyboard_destroy(&server->input);
			outputs_test_unplug(&server->outputs);
		} else if (what && !strcmp(what, "plug")) {
			reply_ok(client);
			outputs_test_plug(&server->outputs);
			if (!server->input.fake) {
				fake_keyboard_create(&server->input);
			}
		} else {
			reply_error(client, "test unplug|plug");
		}
		return;
	}
	if (!strcmp(name, "key")) {
		const char *key = strtok_r(NULL, " \t\r", &save);
		const char *how = strtok_r(NULL, " \t\r", &save);
		if (!getenv("FLAME_TEST_INPUT")) {
			reply_error(client, "start flame with FLAME_TEST_INPUT=1 for this");
		} else if (!key || !how) {
			reply_error(client, "key NAME press|release");
		} else {
			// answer first: the key can be a keybind that quits
			reply_ok(client);
			fake_key(&client->ipc->server->input, key, !strcmp(how, "press"));
		}
		return;
	}
	const char *word = strtok_r(NULL, " \t\r", &save);
	if (word && strtok_r(NULL, " \t\r", &save)) {
		reply_error(client, "too many arguments");
		return;
	}

	if (!strcmp(name, "media-keys")) {
		// the media keys come to this one from now on, as events
		client->media_keys = true;
		reply_ok(client);
		return;
	}
	if (!strcmp(name, "subscribe") || !strcmp(name, "state")) {
		if (word) {
			reply_error(client, "this command takes no argument");
			return;
		}
		client->subscribed |= !strcmp(name, "subscribe");
		send_state(client);
		return;
	}

	for (size_t i = 0; i < LENGTH(commands); i++) {
		if (strcmp(name, commands[i].name)) {
			continue;
		}
		union flame_arg arg = {0};
		const char *error = NULL;
		if (!parse_arg(commands[i].arg, word, &arg, &error)) {
			reply_error(client, error);
			return;
		}
		// answer first: quit tears the socket down
		reply_ok(client);
		commands[i].action(client->ipc->server, &arg);
		return;
	}
	reply_error(client, "unknown command");
}
