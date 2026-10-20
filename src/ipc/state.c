#include "ipc/state.h"

#include <wayland-util.h>

#include "desktop/desktop.h"
#include "util/strbuf.h"

static int window_count(struct flame_workspace *workspace) {
	return wl_list_length(&workspace->views);
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
	strbuf_printf(out, ",\"fullscreen\":%s}", view->fullscreen_wanted ? "true" : "false");
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
	strbuf_printf(out, "}");
}
