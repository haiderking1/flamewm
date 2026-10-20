#include "backend/backend.h"

#include <wlr/backend.h>
#include <wlr/backend/session.h>
#include <wlr/util/log.h>

bool backend_init(struct flame_backend *backend, struct wl_event_loop *loop) {
	backend->wlr = wlr_backend_autocreate(loop, &backend->session);
	if (!backend->wlr) {
		wlr_log(WLR_ERROR, "failed to create backend");
		return false;
	}
	return true;
}

bool backend_start(struct flame_backend *backend) {
	if (!wlr_backend_start(backend->wlr)) {
		wlr_log(WLR_ERROR, "failed to start backend");
		return false;
	}
	return true;
}

void backend_finish(struct flame_backend *backend) {
	wlr_backend_destroy(backend->wlr);
	backend->wlr = NULL;
	backend->session = NULL;
}

bool backend_change_vt(struct flame_backend *backend, unsigned vt) {
	if (!backend->session) {
		return false;
	}
	return wlr_session_change_vt(backend->session, vt);
}
