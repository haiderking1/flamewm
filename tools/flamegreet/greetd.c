#include "greetd.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define PASSWORD_MAX 1024
#define MESSAGE_MAX 65536
#define CMD_MAX 32

static _Atomic enum greet_state state = GREET_IDLE;
static char password[PASSWORD_MAX]; // locked in memory, wiped after each try
static char said[256];
static struct fui *woken;
static struct {
	char user[256];
	char *cmd[CMD_MAX + 1], *env[CMD_MAX + 1];
} job;

// ---- json, as much as greetd's messages need ----

struct out {
	char *data;
	size_t len, size;
	bool failed;
};

static void put(struct out *o, const char *s, size_t n) {
	if (o->failed || o->len + n + 1 > o->size) {
		o->failed = true;
		return;
	}
	memcpy(o->data + o->len, s, n);
	o->len += n;
	o->data[o->len] = '\0';
}

static void put_raw(struct out *o, const char *s) {
	put(o, s, strlen(s));
}

static void put_str(struct out *o, const char *s) {
	put(o, "\"", 1);
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		char esc[8];
		if (c == '"' || c == '\\') {
			esc[0] = '\\';
			esc[1] = (char)c;
			put(o, esc, 2);
		} else if (c < 0x20) {
			snprintf(esc, sizeof(esc), "\\u%04x", c);
			put(o, esc, 6);
		} else {
			put(o, s, 1);
		}
	}
	put(o, "\"", 1);
}

static void put_list(struct out *o, char *const *list) {
	put_raw(o, "[");
	for (int i = 0; list[i]; i++) {
		if (i) {
			put_raw(o, ",");
		}
		put_str(o, list[i]);
	}
	put_raw(o, "]");
}

// key's string value in json (one object deep, which greetd's are), into
// out. false: it isn't there
static bool get_str(const char *json, const char *key, char *out, size_t size) {
	char quoted[64];
	snprintf(quoted, sizeof(quoted), "\"%s\"", key);
	const char *at = json;
	while ((at = strstr(at, quoted))) {
		const char *p = at + strlen(quoted);
		while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
			p++;
		}
		if (*p != ':') {
			at++;
			continue;
		}
		p++;
		while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
			p++;
		}
		if (*p != '"') {
			return false;
		}
		p++;
		size_t n = 0;
		while (*p && *p != '"') {
			char c = *p++;
			if (c == '\\' && *p) {
				c = *p++;
				switch (c) {
				case 'n': c = '\n'; break;
				case 't': c = '\t'; break;
				case 'r': c = '\r'; break;
				case 'b': case 'f': c = ' '; break;
				case 'u': {
					unsigned cp = 0;
					for (int i = 0; i < 4 && *p; i++, p++) {
						char h = *p;
						cp = cp * 16 + (unsigned)(h >= 'a' ? h - 'a' + 10
							: h >= 'A' ? h - 'A' + 10 : h - '0');
					}
					// utf-8 for the basic plane, enough for words
					char utf[4];
					size_t len = 0;
					if (cp < 0x80) {
						utf[len++] = (char)cp;
					} else if (cp < 0x800) {
						utf[len++] = (char)(0xc0 | cp >> 6);
						utf[len++] = (char)(0x80 | (cp & 0x3f));
					} else {
						utf[len++] = (char)(0xe0 | cp >> 12);
						utf[len++] = (char)(0x80 | (cp >> 6 & 0x3f));
						utf[len++] = (char)(0x80 | (cp & 0x3f));
					}
					for (size_t i = 0; i < len && n + 1 < size; i++) {
						out[n++] = utf[i];
					}
					continue;
				}
				default: break;
				}
			}
			if (n + 1 < size) {
				out[n++] = c;
			}
		}
		out[n] = '\0';
		return true;
	}
	return false;
}

// ---- the socket ----

static bool write_all(int fd, const void *data, size_t len) {
	const char *p = data;
	while (len) {
		ssize_t n = write(fd, p, len);
		if (n <= 0) {
			return false;
		}
		p += n;
		len -= (size_t)n;
	}
	return true;
}

static bool read_all(int fd, void *data, size_t len) {
	char *p = data;
	while (len) {
		ssize_t n = read(fd, p, len);
		if (n <= 0) {
			return false;
		}
		p += n;
		len -= (size_t)n;
	}
	return true;
}

// a message: its length (native endian), then the json
static bool send_json(int fd, const struct out *o) {
	uint32_t len = (uint32_t)o->len;
	return !o->failed && write_all(fd, &len, sizeof(len)) && write_all(fd, o->data, o->len);
}

static bool receive(int fd, char *reply, size_t size) {
	uint32_t len;
	if (!read_all(fd, &len, sizeof(len)) || len >= size) {
		return false;
	}
	if (!read_all(fd, reply, len)) {
		return false;
	}
	reply[len] = '\0';
	return true;
}

static bool request(int fd, const char *type, const char *extra_key, const char *extra) {
	char buffer[PASSWORD_MAX * 2 + 512];
	struct out o = {buffer, 0, sizeof(buffer), false};
	put_raw(&o, "{\"type\":");
	put_str(&o, type);
	if (extra_key) {
		put_raw(&o, ",");
		put_str(&o, extra_key);
		put_raw(&o, ":");
		put_str(&o, extra);
	}
	put_raw(&o, "}");
	bool ok = send_json(fd, &o);
	explicit_bzero(buffer, sizeof(buffer)); // a password may have been in it
	return ok;
}

static bool start_session(int fd) {
	static char buffer[MESSAGE_MAX];
	struct out o = {buffer, 0, sizeof(buffer), false};
	put_raw(&o, "{\"type\":\"start_session\",\"cmd\":");
	put_list(&o, job.cmd);
	put_raw(&o, ",\"env\":");
	put_list(&o, job.env);
	put_raw(&o, "}");
	return send_json(fd, &o);
}

static int connect_greetd(void) {
	const char *path = getenv("GREETD_SOCK");
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	if (!path || strlen(path) >= sizeof(addr.sun_path)) {
		return -1;
	}
	strcpy(addr.sun_path, path);
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd >= 0 && connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		fd = -1;
	}
	return fd;
}

static void say(const char *text) {
	snprintf(said, sizeof(said), "%s", text);
}

// the whole conversation: who, what pam asks (the password for what's
// hidden, nothing for what's only said), then the session
static bool converse(int fd) {
	static char reply[MESSAGE_MAX];
	if (!request(fd, "create_session", "username", job.user)) {
		say("Can't reach greetd");
		return false;
	}
	bool answered = false, started = false;
	while (receive(fd, reply, sizeof(reply))) {
		char type[32] = "", kind[32] = "", text[256] = "";
		get_str(reply, "type", type, sizeof(type));
		if (!strcmp(type, "success")) {
			if (started) {
				return true;
			}
			if (!start_session(fd)) {
				say("Can't reach greetd");
				return false;
			}
			started = true;
		} else if (!strcmp(type, "error")) {
			get_str(reply, "error_type", kind, sizeof(kind));
			get_str(reply, "description", text, sizeof(text));
			say(!strcmp(kind, "auth_error") || !*text ? "Wrong password" : text);
			request(fd, "cancel_session", NULL, NULL);
			receive(fd, reply, sizeof(reply));
			return false;
		} else if (!strcmp(type, "auth_message")) {
			get_str(reply, "auth_message_type", kind, sizeof(kind));
			get_str(reply, "auth_message", text, sizeof(text));
			if (!strcmp(kind, "secret") && !answered) {
				answered = true;
				bool sent = request(fd, "post_auth_message_response", "response", password);
				explicit_bzero(password, sizeof(password));
				if (!sent) {
					say("Can't reach greetd");
					return false;
				}
			} else if (!strcmp(kind, "secret") || !strcmp(kind, "visible")) {
				// more than the password (a code, a new one): nothing here
				// answers that
				say(*text ? text : "It asks for more than a password");
				request(fd, "cancel_session", NULL, NULL);
				receive(fd, reply, sizeof(reply));
				return false;
			} else {
				if (*text) {
					say(text); // info, error: shown, nothing to answer
				}
				if (!request(fd, "post_auth_message_response", NULL, NULL)) {
					say("Can't reach greetd");
					return false;
				}
			}
		} else {
			break;
		}
	}
	if (!*said) {
		say("greetd went away");
	}
	return false;
}

static void *login(void *data) {
	int fd = connect_greetd();
	bool ok = false;
	if (fd < 0) {
		say("Can't reach greetd");
	} else {
		ok = converse(fd);
		close(fd);
	}
	explicit_bzero(password, sizeof(password));
	atomic_store(&state, ok ? GREET_OK : GREET_FAILED);
	fui_wake(woken);
	return NULL;
}

static void copy_list(char **to, const char *const *from) {
	for (int i = 0; i < CMD_MAX; i++) {
		free(to[i]);
		to[i] = NULL;
	}
	for (int i = 0; from && from[i] && i < CMD_MAX; i++) {
		to[i] = strdup(from[i]);
		if (!to[i]) {
			break;
		}
	}
}

void greet_login(struct fui *ui, const char *user, const char *typed,
		const struct greet_session *session) {
	if (atomic_load(&state) == GREET_CHECKING) {
		return;
	}
	static bool locked_in;
	if (!locked_in) {
		locked_in = mlock(password, sizeof(password)) == 0;
	}
	snprintf(password, sizeof(password), "%s", typed);
	snprintf(job.user, sizeof(job.user), "%s", user);
	copy_list(job.cmd, session->cmd);
	copy_list(job.env, session->env);
	said[0] = '\0';
	woken = ui;
	atomic_store(&state, GREET_CHECKING);
	pthread_t thread;
	if (pthread_create(&thread, NULL, login, NULL) != 0) {
		explicit_bzero(password, sizeof(password));
		say("Can't log in right now");
		atomic_store(&state, GREET_FAILED);
		return;
	}
	pthread_detach(thread);
}

enum greet_state greet_state(void) {
	return atomic_load(&state);
}

const char *greet_said(void) {
	return atomic_load(&state) == GREET_CHECKING ? "" : said;
}

void greet_again(void) {
	if (atomic_load(&state) == GREET_FAILED) {
		atomic_store(&state, GREET_IDLE);
	}
}
