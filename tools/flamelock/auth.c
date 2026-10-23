#include "auth.h"

#include <pthread.h>
#include <pwd.h>
#include <security/pam_appl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PASSWORD_MAX 1024

static _Atomic enum auth_state state = AUTH_IDLE;
static char password[PASSWORD_MAX]; // locked in memory, wiped after each check
static struct fui *woken;

// pam asking for things: the password for what's hidden, nothing for the rest
static int converse(int n, const struct pam_message **messages, struct pam_response **replies,
		void *data) {
	struct pam_response *out = calloc((size_t)n, sizeof(*out));
	if (!out) {
		return PAM_BUF_ERR;
	}
	for (int i = 0; i < n; i++) {
		int style = messages[i]->msg_style;
		if (style == PAM_PROMPT_ECHO_OFF || style == PAM_PROMPT_ECHO_ON) {
			out[i].resp = strdup(password);
			if (!out[i].resp) {
				for (int k = 0; k < i; k++) {
					free(out[k].resp);
				}
				free(out);
				return PAM_BUF_ERR;
			}
		}
	}
	*replies = out;
	return PAM_SUCCESS;
}

// /etc/pam.d/flamelock (make install puts it there), else login's: what
// checks a password at a console checks it here
static const char *service(void) {
	return access("/etc/pam.d/flamelock", R_OK) == 0 ? "flamelock" : "login";
}

static void *check(void *data) {
	struct passwd *pw = getpwuid(getuid());
	bool ok = false;
	pam_handle_t *pam = NULL;
	const struct pam_conv conv = {converse, NULL};
	if (pw && pam_start(service(), pw->pw_name, &conv, &pam) == PAM_SUCCESS) {
		ok = pam_authenticate(pam, 0) == PAM_SUCCESS;
		pam_end(pam, PAM_SUCCESS);
	}
	explicit_bzero(password, sizeof(password));
	atomic_store(&state, ok ? AUTH_OK : AUTH_FAILED);
	fui_wake(woken);
	return NULL;
}

void auth_check(struct fui *ui, const char *typed) {
	if (atomic_load(&state) == AUTH_CHECKING) {
		return;
	}
	static bool locked_in;
	if (!locked_in) {
		locked_in = mlock(password, sizeof(password)) == 0;
	}
	snprintf(password, sizeof(password), "%s", typed);
	woken = ui;
	atomic_store(&state, AUTH_CHECKING);
	pthread_t thread;
	if (pthread_create(&thread, NULL, check, NULL) != 0) {
		explicit_bzero(password, sizeof(password));
		atomic_store(&state, AUTH_FAILED);
		return;
	}
	pthread_detach(thread);
}

enum auth_state auth_state(void) {
	return atomic_load(&state);
}

void auth_again(void) {
	if (atomic_load(&state) == AUTH_FAILED) {
		atomic_store(&state, AUTH_IDLE);
	}
}
