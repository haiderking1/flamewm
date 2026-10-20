#include "ipc/commands.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "bindings/action.h"
#include "core/server.h"
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

void ipc_command(struct flame_ipc_client *client, char *line) {
	char *save = NULL;
	const char *name = strtok_r(line, " \t\r", &save);
	if (!name) {
		return; // empty line, nothing to answer
	}
	const char *word = strtok_r(NULL, " \t\r", &save);
	if (word && strtok_r(NULL, " \t\r", &save)) {
		reply_error(client, "too many arguments");
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
