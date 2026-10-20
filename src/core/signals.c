#include "core/signals.h"

#include <signal.h>
#include <wlr/util/log.h>

#include "core/server.h"

static int handle_terminate(int signal_number, void *data) {
	struct flame_server *server = data;
	wlr_log(WLR_INFO, "got signal %d, shutting down", signal_number);
	server_terminate(server);
	return 0;
}

bool signals_init(struct flame_signals *signals, struct flame_server *server) {
	// a client hanging up mid write shouldn't take the compositor with it
	signal(SIGPIPE, SIG_IGN);

	signals->sigint = wl_event_loop_add_signal(server->loop, SIGINT,
		handle_terminate, server);
	signals->sigterm = wl_event_loop_add_signal(server->loop, SIGTERM,
		handle_terminate, server);
	if (!signals->sigint || !signals->sigterm) {
		wlr_log(WLR_ERROR, "failed to watch SIGINT/SIGTERM");
		return false;
	}
	return true;
}

void signals_finish(struct flame_signals *signals) {
	wl_event_source_remove(signals->sigint);
	wl_event_source_remove(signals->sigterm);
}
