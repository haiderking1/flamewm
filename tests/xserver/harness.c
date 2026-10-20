// flame's X server on its own, no wayland, no gpu: runs a test client
// against it and plays the part of flame. the client steers through a pipe,
// a line per command, and waits for "ok" before looking at what X sent it:
//   motion X Y        the pointer moves to a root point
//   button N 1|0      press or release
//   scroll v|h DELTA  wayland axis units, 15 a notch
//   raw DX DY         the mouse moved this much
//   key CODE 1|0      an X keycode
//   repeat CODE       the held key repeats
//   focus WINDOW      flame moves keyboard focus
//   monitors N        N outputs side by side, 1920x1080 each, 165Hz
//   layouts L         a keymap with these layouts ("us,ara")
//   mods D L K G      modifiers depressed, latched, locked, and the layout
//   own SEL, disown SEL   flame takes a selection or lets it go. asked for
//                     it, flame answers TARGETS and UTF8_STRING ("flame
//                     clipboard"), nothing else
//   convert SEL TARGET    flame asks the X owner, the answer comes back as
//                     "ok TYPE LENGTH FIRST-BYTES" once it's all there
//   owner SEL         "ok 1" when an X client owns it, as flame last heard
// every command is answered with a line starting with "ok"
// usage: harness test-client [args], exits with the client's status
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

#include "xserver/internal.h"

struct harness {
	struct flame_xserver xserver;
	struct wl_event_loop *loop;
	struct xkb_context *context;
	int commands, replies; // the pipe ends, ours
	struct wl_event_source *command_source;
	char line[256];
	size_t line_len;
	uint16_t state;
	pid_t child;
	// selections: what flame heard about owners, the answer being collected
	char owners[256];
	char *answer;
	size_t answer_len;
	bool converting;
	int status;
	bool done;
};

static void pointer(void *data, double *x, double *y) {
	struct harness *h = data;
	*x = h->xserver.pointer_x;
	*y = h->xserver.pointer_y;
}

static uint32_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static struct xs_window *top_at(struct flame_xserver *xserver, int x, int y) {
	return xs_window_child_at(xserver->root, x, y);
}

static void reply(struct harness *h, const char *line) {
	size_t len = strlen(line);
	if (write(h->replies, line, len) != (ssize_t)len) {
		h->done = true;
	}
}

static void selection_owner(void *data, uint32_t selection, bool x_owned) {
	struct harness *h = data;
	const char *name = xserver_atom_name(&h->xserver, selection);
	char entry[96];
	snprintf(entry, sizeof(entry), " %s=%d", name ? name : "?", x_owned);
	if (strlen(h->owners) + strlen(entry) < sizeof(h->owners)) {
		strcat(h->owners, entry);
	}
}

static void selection_request(void *data, struct xs_selection_request *request) {
	struct harness *h = data;
	struct flame_xserver *x = &h->xserver;
	static const char text[] = "flame clipboard";
	if (request->target == xserver_atom(x, "TARGETS")) {
		uint32_t targets[] = {xserver_atom(x, "TARGETS"), xserver_atom(x, "UTF8_STRING")};
		xserver_selection_answer(x, request, xserver_atom(x, "ATOM"), 32, targets,
			sizeof(targets));
	} else if (request->target == xserver_atom(x, "UTF8_STRING")) {
		xserver_selection_answer(x, request, request->target, 8, text, sizeof(text) - 1);
	} else {
		xserver_selection_answer(x, request, 0, 8, NULL, 0);
	}
}

static void converted(void *data, uint32_t type, const uint8_t *bytes, size_t len, bool last) {
	struct harness *h = data;
	char *grown = realloc(h->answer, h->answer_len + len + 1);
	if (grown && len) {
		memcpy(grown + h->answer_len, bytes, len);
		h->answer = grown;
		h->answer_len += len;
		grown[h->answer_len] = '\0';
	} else if (grown) {
		h->answer = grown;
		grown[h->answer_len] = '\0';
	}
	if (!last) {
		return;
	}
	const char *name = type ? xserver_atom_name(&h->xserver, type) : "None";
	char line[128];
	snprintf(line, sizeof(line), "ok %s %zu %.32s\n", name ? name : "?", h->answer_len,
		h->answer ? h->answer : "");
	for (char *c = line + 3; *c && c[1]; c++) {
		*c = *c == '\n' ? ' ' : *c;
	}
	free(h->answer);
	h->answer = NULL;
	h->answer_len = 0;
	h->converting = false;
	reply(h, line);
}

static void command(struct harness *h, char *line) {
	struct flame_xserver *x = &h->xserver;
	char word[16] = "", axis = 0;
	int a = 0, b = 0;
	double d1 = 0, d2 = 0;
	unsigned window = 0;
	sscanf(line, "%15s", word);
	if (sscanf(line, "motion %d %d", &a, &b) == 2) {
		xserver_pointer_motion(x, top_at(x, a, b), a, b, h->state, now_ms());
	} else if (sscanf(line, "button %d %d", &a, &b) == 2) {
		uint16_t state = h->state;
		if (a >= 1 && a <= 5) {
			uint16_t bit = (uint16_t)(1u << (7 + a));
			h->state = b ? h->state | bit : h->state & (uint16_t)~bit;
		}
		xserver_pointer_button(x, (uint8_t)a, b, state, now_ms());
	} else if (sscanf(line, "scroll %c %lf", &axis, &d1) == 2) {
		xserver_pointer_scroll(x, axis == 'v', d1, h->state, now_ms());
	} else if (sscanf(line, "raw %lf %lf", &d1, &d2) == 2) {
		xserver_pointer_raw_motion(x, d1, d2, d1, d2, now_ms());
	} else if (sscanf(line, "key %d %d", &a, &b) == 2) {
		xserver_key(x, (uint8_t)a, b, h->state, now_ms());
	} else if (sscanf(line, "repeat %d", &a) == 1) {
		xserver_key_repeat(x, (uint8_t)a, h->state, now_ms());
	} else if (sscanf(line, "focus %x", &window) == 1) {
		xserver_set_focus(x, window);
	} else if (sscanf(line, "monitors %d", &a) == 1) {
		struct xs_monitor monitors[8] = {0};
		for (int i = 0; i < a && i < 8; i++) {
			snprintf(monitors[i].name, sizeof(monitors[i].name), "DP-%d", i + 1);
			monitors[i].x = i * 1920;
			monitors[i].width = 1920;
			monitors[i].height = 1080;
			monitors[i].mm_width = 527;
			monitors[i].mm_height = 296;
			monitors[i].refresh = 165000;
			monitors[i].primary = i == 0;
		}
		xserver_set_monitors(x, monitors, (size_t)(a < 8 ? a : 8));
	} else if (!strcmp(word, "layouts")) {
		char layouts[64] = "";
		sscanf(line, "layouts %63s", layouts);
		struct xkb_rule_names names = {.layout = layouts};
		struct xkb_keymap *keymap = xkb_keymap_new_from_names(h->context, &names,
			XKB_KEYMAP_COMPILE_NO_FLAGS);
		xserver_set_keymap(x, keymap);
		xkb_keymap_unref(keymap);
	} else if (!strcmp(word, "mods")) {
		unsigned d = 0, l = 0, k = 0, g = 0;
		sscanf(line, "mods %u %u %u %u", &d, &l, &k, &g);
		h->state = (uint16_t)((h->state & 0x1f00) | ((d | l | k) & 0xff) | (g & 3) << 13);
		xserver_keyboard_state(x, d, l, k, g);
	} else if (!strcmp(word, "own") || !strcmp(word, "disown") || !strcmp(word, "convert")
			|| !strcmp(word, "owner")) {
		char selection[64] = "", target[64] = "";
		sscanf(line, "%*s %63s %63s", selection, target);
		uint32_t atom = xserver_atom(x, selection);
		if (!strcmp(word, "convert")) {
			if (xserver_selection_convert(x, atom, xserver_atom(x, target), converted, h)) {
				h->converting = true;
				return; // the answer replies
			}
			reply(h, "ok None 0\n");
			return;
		}
		if (!strcmp(word, "owner")) {
			char out[300];
			snprintf(out, sizeof(out), "ok %d%s\n", xserver_selection_owned(x, atom), h->owners);
			h->owners[0] = '\0';
			reply(h, out);
			return;
		}
		xserver_selection_own(x, atom, !strcmp(word, "own"));
	} else {
		fprintf(stderr, "harness: what's '%s'\n", line);
	}
	reply(h, "ok\n");
}

static int handle_command(int fd, uint32_t mask, void *data) {
	struct harness *h = data;
	char buf[256];
	ssize_t n = read(fd, buf, sizeof(buf));
	if (n <= 0) {
		wl_event_source_remove(h->command_source);
		h->command_source = NULL;
		return 0;
	}
	for (ssize_t i = 0; i < n; i++) {
		if (buf[i] == '\n' || h->line_len == sizeof(h->line) - 1) {
			h->line[h->line_len] = '\0';
			command(h, h->line);
			h->line_len = 0;
		} else {
			h->line[h->line_len++] = buf[i];
		}
	}
	return 0;
}

static int handle_child(int signal, void *data) {
	struct harness *h = data;
	if (waitpid(h->child, &h->status, WNOHANG) == h->child) {
		h->done = true;
	}
	return 0;
}

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: %s test-client [args]\n", argv[0]);
		return 2;
	}
	wlr_log_init(getenv("HARNESS_DEBUG") ? WLR_DEBUG : WLR_ERROR, NULL);
	struct harness h = {0};
	h.context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	h.loop = wl_event_loop_create();
	struct xs_hooks hooks = {
		.pointer = pointer,
		.selection_owner = selection_owner,
		.selection_request = selection_request,
		.data = &h,
	};
	if (!h.context || !xserver_init(&h.xserver, h.loop, hooks, 1920, 1080)
			|| !xserver_start(&h.xserver)) {
		fprintf(stderr, "harness: no X server\n");
		return 2;
	}
	int to_harness[2], to_client[2];
	if (pipe(to_harness) || pipe(to_client)) {
		return 2;
	}
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGCHLD);
	sigprocmask(SIG_BLOCK, &mask, NULL);
	struct wl_event_source *child_source = wl_event_loop_add_signal(h.loop, SIGCHLD,
		handle_child, &h);

	h.child = fork();
	if (h.child == 0) {
		sigprocmask(SIG_UNBLOCK, &mask, NULL);
		char display[16], fds[32];
		snprintf(display, sizeof(display), ":%d", h.xserver.display);
		snprintf(fds, sizeof(fds), "%d %d", to_harness[1], to_client[0]);
		setenv("DISPLAY", display, 1);
		setenv("HARNESS_FDS", fds, 1);
		close(to_harness[0]);
		close(to_client[1]);
		execvp(argv[1], argv + 1);
		_exit(127);
	}
	close(to_harness[1]);
	close(to_client[0]);
	h.commands = to_harness[0];
	h.replies = to_client[1];
	h.command_source = wl_event_loop_add_fd(h.loop, h.commands, WL_EVENT_READABLE,
		handle_command, &h);
	while (!h.done) {
		wl_event_loop_dispatch(h.loop, -1);
	}
	if (h.command_source) {
		wl_event_source_remove(h.command_source);
	}
	wl_event_source_remove(child_source);
	close(h.commands);
	close(h.replies);
	xserver_finish(&h.xserver);
	wl_event_loop_destroy(h.loop);
	xkb_context_unref(h.context);
	if (WIFSIGNALED(h.status)) {
		fprintf(stderr, "harness: client died of signal %d\n", WTERMSIG(h.status));
		return 1;
	}
	return WEXITSTATUS(h.status);
}
