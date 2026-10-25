#include "lock/sleep.h"

#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>
#include <wlr/util/log.h>

#include "core/server.h"
#include "util/spawn.h"

// locked before the computer sleeps: logind's told flame wants a moment
// first (a delay inhibitor, held all the time it's awake). going to sleep,
// flamelock starts, and the moment's over once every screen shows it
// locked (or after a few seconds, whatever happened): opened again, nobody
// sees the desktop before the lock

#define LOGIND "org.freedesktop.login1"
#define LOGIND_PATH "/org/freedesktop/login1"
#define LOGIND_MANAGER "org.freedesktop.login1.Manager"
#define WAIT_MS 3000 // the longest sleep waits for the lock

static void pump(struct flame_sleep *sleep) {
	while (sd_bus_process(sleep->bus, NULL) > 0) {
	}
	int events = sd_bus_get_events(sleep->bus);
	wl_event_source_fd_update(sleep->source, (events & POLLIN ? WL_EVENT_READABLE : 0)
		| (events & POLLOUT ? WL_EVENT_WRITABLE : 0));
	uint64_t until;
	if (sd_bus_get_timeout(sleep->bus, &until) >= 0 && until != UINT64_MAX) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		uint64_t now_us = (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
		wl_event_source_timer_update(sleep->timer,
			until > now_us ? (int)((until - now_us + 999) / 1000) : 1);
	}
}

static int handle_bus(int fd, uint32_t mask, void *data) {
	pump(data);
	return 0;
}

static int handle_timer(void *data) {
	pump(data);
	return 0;
}

// the moment's over: sleep goes on
static void let_go(struct flame_sleep *sleep) {
	if (sleep->inhibitor >= 0) {
		close(sleep->inhibitor);
		sleep->inhibitor = -1;
	}
	if (sleep->waiting) {
		wl_event_source_timer_update(sleep->give_up, 0);
		sleep->waiting = false;
	}
}

// a moment before each sleep, asked for again after each
static void hold(struct flame_sleep *sleep) {
	if (sleep->inhibitor >= 0) {
		return;
	}
	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = NULL;
	int fd = -1;
	int r = sd_bus_call_method(sleep->bus, LOGIND, LOGIND_PATH, LOGIND_MANAGER, "Inhibit",
		&error, &reply, "ssss", "sleep", "flamewm", "the screen locks before sleeping",
		"delay");
	if (r >= 0 && sd_bus_message_read(reply, "h", &fd) >= 0) {
		sleep->inhibitor = fcntl(fd, F_DUPFD_CLOEXEC, 3); // the reply's goes with it
	} else {
		wlr_log(WLR_ERROR, "sleep: logind won't wait for the lock: %s",
			error.message ? error.message : strerror(-r));
	}
	sd_bus_error_free(&error);
	sd_bus_message_unref(reply);
}

static int handle_give_up(void *data) {
	struct flame_sleep *sleep = data;
	wlr_log(WLR_ERROR, "sleep: no lock in time, sleeping anyway");
	sleep->waiting = false;
	let_go(sleep);
	return 0;
}

static int prepare_for_sleep(sd_bus_message *m, void *data, sd_bus_error *error) {
	struct flame_sleep *sleep = data;
	int going;
	if (sd_bus_message_read(m, "b", &going) < 0) {
		return 0;
	}
	if (!going) {
		hold(sleep); // awake: the next one waits too
		return 0;
	}
	struct flame_lock *lock = &sleep->server->lock;
	if (lock_active(lock) && lock->told) {
		let_go(sleep); // already locked, nothing to wait for
		return 0;
	}
	const char *command = config_lock_command(sleep->server->config);
	if (!lock_active(lock) && !command) {
		let_go(sleep); // nothing locks here (lock = ""), nothing to wait for
		return 0;
	}
	wlr_log(WLR_INFO, "sleep: locking first");
	if (!lock_active(lock)) {
		spawn_shell(command);
	}
	sleep->waiting = true;
	wl_event_source_timer_update(sleep->give_up, WAIT_MS);
	return 0;
}

void sleep_locked(struct flame_sleep *sleep) {
	if (sleep->waiting) {
		let_go(sleep);
	}
}

bool sleep_init(struct flame_sleep *sleep, struct flame_server *server) {
	sleep->server = server;
	sleep->inhibitor = -1;
	if (!server->backend.session) {
		return true; // nested or headless: the host's sleep isn't ours
	}
	int r = sd_bus_open_system(&sleep->bus);
	if (r < 0) {
		wlr_log(WLR_ERROR, "sleep: no system bus, the lock won't come before sleep: %s",
			strerror(-r));
		sleep->bus = NULL;
		return true;
	}
	r = sd_bus_match_signal(sleep->bus, &sleep->slot, LOGIND, LOGIND_PATH, LOGIND_MANAGER,
		"PrepareForSleep", prepare_for_sleep, sleep);
	sleep->source = wl_event_loop_add_fd(server->loop, sd_bus_get_fd(sleep->bus),
		WL_EVENT_READABLE, handle_bus, sleep);
	sleep->timer = wl_event_loop_add_timer(server->loop, handle_timer, sleep);
	sleep->give_up = wl_event_loop_add_timer(server->loop, handle_give_up, sleep);
	if (r < 0 || !sleep->source || !sleep->timer || !sleep->give_up) {
		wlr_log(WLR_ERROR, "sleep: can't hear logind, the lock won't come before sleep");
		sleep_finish(sleep);
		return true;
	}
	hold(sleep);
	pump(sleep);
	return true;
}

void sleep_finish(struct flame_sleep *sleep) {
	let_go(sleep);
	if (sleep->give_up) {
		wl_event_source_remove(sleep->give_up);
		sleep->give_up = NULL;
	}
	if (sleep->timer) {
		wl_event_source_remove(sleep->timer);
		sleep->timer = NULL;
	}
	if (sleep->source) {
		wl_event_source_remove(sleep->source);
		sleep->source = NULL;
	}
	sd_bus_slot_unref(sleep->slot);
	sleep->slot = NULL;
	if (sleep->bus) {
		sd_bus_flush_close_unref(sleep->bus);
		sleep->bus = NULL;
	}
}
