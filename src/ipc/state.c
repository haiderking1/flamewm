#include "ipc/state.h"

#include <wayland-util.h>

#include "core/server.h"
#include "desktop/desktop.h"
#include "util/strbuf.h"

static int window_count(struct flame_workspace *workspace) {
	return workspace_view_count(workspace);
}

static void write_window(struct strbuf *out, struct flame_view *view) {
	if (!view) {
		strbuf_printf(out, "null");
		return;
	}
	strbuf_printf(out, "{\"app_id\":");
	strbuf_json_string(out, view_app_id(view));
	strbuf_printf(out, ",\"title\":");
	strbuf_json_string(out, view_title(view));
	strbuf_printf(out, ",\"fullscreen\":%s,\"floating\":%s", view->fullscreen_wanted ? "true" : "false",
		view->floating ? "true" : "false");
	struct wlr_box box = view->geometry;
	strbuf_printf(out, ",\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d}", box.x, box.y, box.width,
		box.height);
}

// each screen and how its frames go out: scanout, a client's buffer shown by
// KMS itself with nothing composed
static void write_outputs(struct flame_server *server, struct strbuf *out) {
	strbuf_printf(out, ",\"outputs\":[");
	struct flame_output *output;
	bool first = true;
	wl_list_for_each(output, &server->outputs.list, link) {
		strbuf_printf(out, "%s{\"name\":", first ? "" : ",");
		strbuf_json_string(out, output->wlr->name);
		strbuf_printf(out, ",\"scanout\":%s,\"tearing\":%s}",
			output->frames.scanout.active ? "true" : "false",
			output->frames.sync.tearing ? "true" : "false");
		first = false;
	}
	strbuf_printf(out, "]");
}

void ipc_state_write(struct flame_desktop *desktop, struct strbuf *out) {
	// ids are what you press: 1..9, then 10 on the 0 key
	strbuf_printf(out, "{\"event\":\"state\",\"workspace\":%d,\"workspaces\":[",
		desktop->active->index + 1);
	for (int i = 0; i < FLAME_WORKSPACE_COUNT; i++) {
		struct flame_workspace *workspace = &desktop->workspaces[i];
		strbuf_printf(out, "%s{\"id\":%d,\"name\":\"%d\",\"windows\":%d,"
			"\"active\":%s,\"fullscreen\":%s}", i ? "," : "", i + 1, i + 1,
			window_count(workspace), workspace == desktop->active ? "true" : "false",
			workspace->fullscreen ? "true" : "false");
	}
	strbuf_printf(out, "],\"window\":");
	write_window(out, desktop->focused);
	write_outputs(desktop->server, out);
	strbuf_printf(out, "}");
}
