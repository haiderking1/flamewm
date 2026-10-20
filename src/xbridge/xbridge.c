#include "xbridge/xbridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/pidfd.h>
#include <unistd.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/log.h>

#include "core/server.h"
#include "util/listener.h"
#include "util/macros.h"

// whose wayland toplevels are views of their X windows
static const char *const bridged[] = {"steamwebhelper"};
// whose X windows are never shown: steam's UI is on wayland, the rest of its
// X windows (the updater's, its old GL surface) are leftovers nobody sees on X
static const char *const hidden[] = {"steam", "steamwebhelper"};

static bool listed(const char *const *names, size_t count, const char *comm) {
	for (size_t i = 0; i < count; i++) {
		if (!strcmp(names[i], comm)) {
			return true;
		}
	}
	return false;
}

static struct flame_xbridge_process *find(struct flame_xbridge *xbridge, pid_t pid) {
	struct flame_xbridge_process *process;
	wl_list_for_each(process, &xbridge->processes, link) {
		if (process->pid == pid) {
			return process;
		}
	}
	return NULL;
}

static struct flame_xbridge_window *window_at(struct flame_xbridge_process *process,
		uint32_t index) {
	struct flame_xbridge_window *window;
	wl_list_for_each(window, &process->windows, link) {
		if (window->index == index) {
			return window;
		}
	}
	return NULL;
}

// ---- verdicts ----

static struct xs_window *view_window(struct flame_xbridge_process *process, uint32_t index) {
	if (index == 0 || index > process->views_len || !process->views[index - 1]) {
		return NULL;
	}
	return xserver_window(&process->xbridge->xserver, process->views[index - 1]);
}

// the first of the process's views still inside top, 0 for none
static uint32_t primary_view(struct flame_xbridge_process *process, struct xs_window *top) {
	for (uint32_t i = 1; i <= process->views_len; i++) {
		struct xs_window *view = view_window(process, i);
		if (view && view->parent == top) {
			return i;
		}
	}
	return 0;
}

static void compute(struct flame_xbridge_process *process, uint32_t index,
		struct flame_xbridge_verdict *out, char *title, size_t title_size) {
	*out = (struct flame_xbridge_verdict){.role = FLAME_XBRIDGE_HIDDEN};
	struct xs_window *view = view_window(process, index);
	struct xs_window *top = view ? view->parent : NULL;
	if (!view || !view->mapped || !top || !xs_window_top_level(top) || !top->mapped) {
		return;
	}
	uint32_t primary = primary_view(process, top);
	if (primary != index) {
		// a second view drawn over the first. 1x1 is how steam parks one
		if (!primary || view->width <= 2 || view->height <= 2) {
			return;
		}
		out->role = FLAME_XBRIDGE_EMBED;
		out->box = (struct wlr_box){view->x, view->y, view->width, view->height};
		out->host = primary;
		return;
	}
	out->role = top->override_redirect ? FLAME_XBRIDGE_POPUP : FLAME_XBRIDGE_WINDOW;
	out->box = (struct wlr_box){top->x, top->y, top->width, top->height};
	out->title = (char *)xs_window_title(&process->xbridge->xserver, top, title, title_size);
}

// grabs isn't compared: it's decided once, when the popup opens
static bool same_verdict(const struct flame_xbridge_verdict *a,
		const struct flame_xbridge_verdict *b) {
	if (a->role != b->role || a->host != b->host || !wlr_box_equal(&a->box, &b->box)) {
		return false;
	}
	if (!a->title || !b->title) {
		return a->title == b->title;
	}
	return !strcmp(a->title, b->title);
}

// the X side changed: every view's verdict again, toplevels hear about theirs
static void refresh(struct flame_xbridge_process *process) {
	if (process->views_len > process->verdicts_len) {
		struct flame_xbridge_verdict *verdicts =
			realloc(process->verdicts, process->views_len * sizeof(*verdicts));
		if (!verdicts) {
			wlr_log(WLR_ERROR, "out of memory");
			return;
		}
		for (size_t i = process->verdicts_len; i < process->views_len; i++) {
			verdicts[i] = (struct flame_xbridge_verdict){.role = FLAME_XBRIDGE_HIDDEN};
		}
		process->verdicts = verdicts;
		process->verdicts_len = process->views_len;
	}
	for (uint32_t index = 1; index <= process->views_len; index++) {
		char title[256];
		struct flame_xbridge_verdict verdict;
		compute(process, index, &verdict, title, sizeof(title));
		struct flame_xbridge_verdict *current = &process->verdicts[index - 1];
		if (same_verdict(&verdict, current)) {
			continue;
		}
		char *copy = verdict.title ? strdup(verdict.title) : NULL;
		bool opening = verdict.role == FLAME_XBRIDGE_POPUP
			&& current->role != FLAME_XBRIDGE_POPUP;
		bool grabs = opening ? process->xbridge->clicked : current->grabs;
		free(current->title);
		*current = verdict;
		current->title = copy;
		current->grabs = verdict.role == FLAME_XBRIDGE_POPUP && grabs;
		struct flame_xbridge_window *window = window_at(process, index);
		if (window) {
			window->changed(window);
		}
	}
}

// ---- focus, both ways ----

// X focus moved between a process's windows: activation and the keyboard
// follow, like X11 apps get FocusIn and FocusOut
static void apply_focus(struct flame_xbridge *xbridge, struct flame_xbridge_process *process) {
	struct flame_xbridge_window *window, *tmp;
	wl_list_for_each_safe(window, tmp, &process->windows, link) {
		window->changed(window);
	}
	// keyboard routing asks the focused view, which may be one of these
	struct flame_desktop *desktop = &xbridge->server->desktop;
	if (desktop->focused) {
		view_set_focused(desktop->focused, true);
	}
	focus_restore_keyboard(desktop);
}

// which of its views an X window is: a view itself, or a top-level's first
static uint32_t view_of(struct flame_xbridge_process *process, struct xs_window *window) {
	if (!window) {
		return 0;
	}
	for (uint32_t i = 1; i <= process->views_len; i++) {
		if (process->views[i - 1] == window->id) {
			return i;
		}
	}
	return xs_window_top_level(window) ? primary_view(process, window) : 0;
}

// X focus is on focus now, whoever moved it: every process learns which of
// its views has it, if any
static void focus_moved(struct flame_xbridge *xbridge, uint32_t focus) {
	struct xs_window *window = xserver_window(&xbridge->xserver, focus);
	struct flame_xbridge_process *process;
	wl_list_for_each(process, &xbridge->processes, link) {
		if (!process->claimed) {
			continue;
		}
		uint32_t index = window && window->owner && window->owner->pid == process->pid
			? view_of(process, window) : 0;
		if (index != process->focus) {
			wlr_log(WLR_DEBUG, "xbridge: X focus 0x%x, pid %d view #%u -> #%u", focus,
				(int)process->pid, process->focus, index);
			process->focus = index;
			apply_focus(xbridge, process);
		}
	}
}

static struct flame_xbridge_window *find_window(struct flame_xbridge *xbridge,
		const void *view) {
	struct flame_xbridge_process *process;
	wl_list_for_each(process, &xbridge->processes, link) {
		struct flame_xbridge_window *window;
		wl_list_for_each(window, &process->windows, link) {
			if (window->view == view) {
				return window;
			}
		}
	}
	return NULL;
}

// flame focused something: an X app gets FocusIn on the window flame picked,
// or FocusOut when flame went elsewhere. only when flame's focus really
// moved: what the app does with X focus inside its own windows is its
// business. steam parks it on None while switching menus, and pulling it
// back onto the main window makes that window think it's focused, so its
// tab hover dropdowns pop up and close the open menu
static void handle_desktop_changed(struct wl_listener *listener, void *data) {
	struct flame_xbridge *xbridge = wl_container_of(listener, xbridge, desktop_changed);
	struct flame_xserver *xserver = &xbridge->xserver;
	const void *flame_focus = xbridge->server->desktop.focused;
	if (flame_focus == xbridge->flame_focus) {
		return;
	}
	xbridge->flame_focus = flame_focus;
	struct flame_xbridge_window *focused = find_window(xbridge, flame_focus);
	struct xs_window *current = xserver_window(xserver, xserver->focus);
	pid_t current_pid = current && current->owner ? current->owner->pid : 0;

	if (focused) {
		struct flame_xbridge_process *process = focused->process;
		if (current_pid == process->pid) {
			return; // already somewhere in this app
		}
		struct xs_window *view = view_window(process, focused->index);
		wlr_log(WLR_DEBUG, "xbridge: flame focused pid %d view #%u, X focus follows",
			(int)process->pid, focused->index);
		if (view && xs_window_viewable(view)) {
			xserver_set_focus(xserver, view->id);
			focus_moved(xbridge, view->id);
		}
		return;
	}
	struct flame_xbridge_process *current_process = current_pid ? find(xbridge, current_pid) : NULL;
	if (current_process && current_process->claimed) {
		wlr_log(WLR_DEBUG, "xbridge: flame focused something else, X focus off pid %d",
			(int)current_pid);
		xserver_set_focus(xserver, XS_FOCUS_NONE);
		focus_moved(xbridge, XS_FOCUS_NONE);
	}
}

// ---- placement ----

void xbridge_window_placed(struct flame_xbridge_window *window, struct wlr_box box) {
	struct flame_xbridge_process *process = window->process;
	if (!process || wlr_box_equal(&box, &window->placed)) {
		return;
	}
	window->placed = box;
	// only a tile's own top-level follows it, menus and embeds stay where
	// the app put them
	struct xs_window *view = view_window(process, window->index);
	struct xs_window *top = view ? view->parent : NULL;
	if (!top || !xs_window_top_level(top) || top->override_redirect
			|| primary_view(process, top) != window->index) {
		return;
	}
	xserver_move_window(&process->xbridge->xserver, top, box.x, box.y,
		MAX(box.width, 1), MAX(box.height, 1));
}

// ---- toplevels ----

void xbridge_window_attach(struct flame_xbridge *xbridge, struct flame_xbridge_window *window,
		struct wl_client *client, const void *view,
		void (*changed)(struct flame_xbridge_window *window)) {
	*window = (struct flame_xbridge_window){.changed = changed, .view = view};
	wl_list_init(&window->link);
	pid_t pid;
	wl_client_get_credentials(client, &pid, NULL, NULL);
	struct flame_xbridge_process *process = find(xbridge, pid);
	if (!process || !process->claimed) {
		return;
	}
	window->process = process;
	window->index = ++process->created;
	wl_list_insert(process->windows.prev, &window->link);
	wlr_log(WLR_DEBUG, "xbridge: pid %d toplevel #%u", (int)pid, window->index);
}

void xbridge_window_detach(struct flame_xbridge_window *window) {
	wl_list_remove(&window->link);
	wl_list_init(&window->link);
	window->process = NULL;
}

const struct flame_xbridge_verdict *xbridge_window_verdict(
		const struct flame_xbridge_window *window) {
	static const struct flame_xbridge_verdict unknown = {.role = FLAME_XBRIDGE_HIDDEN};
	struct flame_xbridge_process *process = window->process;
	if (!process) {
		return NULL;
	}
	// no view for it yet is an X window that isn't mapped yet
	if (window->index > process->verdicts_len) {
		return &unknown;
	}
	return &process->verdicts[window->index - 1];
}

struct flame_xbridge_window *xbridge_window_sibling(struct flame_xbridge_window *window,
		uint32_t index) {
	return window->process ? window_at(window->process, index) : NULL;
}

void xbridge_window_embeds_changed(struct flame_xbridge_window *window) {
	struct flame_xbridge_process *process = window->process;
	if (!process) {
		return;
	}
	struct flame_xbridge_window *embed, *tmp;
	wl_list_for_each_safe(embed, tmp, &process->windows, link) {
		const struct flame_xbridge_verdict *verdict = xbridge_window_verdict(embed);
		if (verdict->role == FLAME_XBRIDGE_EMBED && verdict->host == window->index) {
			embed->changed(embed);
		}
	}
}

bool xbridge_window_x_focused(const struct flame_xbridge_window *window) {
	return window->process && window->index == window->process->focus;
}

struct flame_xbridge_window *xbridge_window_x_focus(struct flame_xbridge_window *window) {
	return window->process && window->process->focus
		? window_at(window->process, window->process->focus) : NULL;
}

bool xbridge_menu_holds_pointer(struct flame_xbridge *xbridge, struct wlr_surface *surface) {
	pid_t pid;
	wl_client_get_credentials(wl_resource_get_client(surface->resource), &pid, NULL, NULL);
	struct flame_xbridge_process *process = find(xbridge, pid);
	if (!process || !process->claimed || !process->focus) {
		return false;
	}
	struct flame_xbridge_window *menu = window_at(process, process->focus);
	const struct flame_xbridge_verdict *verdict = menu ? xbridge_window_verdict(menu) : NULL;
	if (!verdict || verdict->role != FLAME_XBRIDGE_POPUP || !verdict->grabs) {
		return false;
	}
	struct wlr_surface *menu_surface = view_surface((struct flame_view *)menu->view);
	return wlr_surface_get_root_surface(surface) != menu_surface;
}

void xbridge_pointer_input(struct flame_xbridge *xbridge, bool button) {
	xbridge->clicked = button;
}

void xbridge_pointer_pressed(struct flame_xbridge *xbridge, struct wlr_surface *surface) {
	struct wlr_surface *root = wlr_surface_get_root_surface(surface);
	struct flame_xbridge_process *process;
	wl_list_for_each(process, &xbridge->processes, link) {
		struct flame_xbridge_window *window;
		wl_list_for_each(window, &process->windows, link) {
			if (view_surface((struct flame_view *)window->view) != root) {
				continue;
			}
			if (process->focus == window->index) {
				return; // a click inside the menu keeps it open
			}
			struct xs_window *view = view_window(process, window->index);
			if (view && xs_window_viewable(view)) {
				wlr_log(WLR_DEBUG, "xbridge: click on pid %d view #%u takes X focus",
					(int)process->pid, window->index);
				xserver_set_focus(&xbridge->xserver, view->id);
				focus_moved(xbridge, view->id);
			}
			return;
		}
	}
}

// ---- the X server's side ----

bool xbridge_hides(struct flame_xbridge *xbridge, pid_t pid) {
	struct flame_xbridge_process *process = find(xbridge, pid);
	return process && (process->claimed || listed(hidden, LENGTH(hidden), process->comm));
}

static void release(struct flame_xbridge_process *process);

static int handle_process_exit(int fd, uint32_t mask, void *data) {
	struct flame_xbridge_process *process = data;
	wlr_log(WLR_DEBUG, "xbridge: pid %d exited", (int)process->pid);
	release(process);
	return 0;
}

static void hook_connected(void *data, struct xs_client *client) {
	struct flame_xbridge *xbridge = data;
	struct flame_xbridge_process *process = find(xbridge, client->pid);
	if (!process) {
		process = calloc(1, sizeof(*process));
		if (!process) {
			wlr_log(WLR_ERROR, "out of memory");
			return;
		}
		process->xbridge = xbridge;
		process->pid = client->pid;
		process->claimed = listed(bridged, LENGTH(bridged), client->comm);
		snprintf(process->comm, sizeof(process->comm), "%s", client->comm);
		wl_list_init(&process->windows);
		// apps open and close probe connections all the time, a process is
		// forgotten when it exits, not when its last connection closes
		process->pidfd = pidfd_open(client->pid, 0);
		if (process->pidfd >= 0) {
			process->exit_source = wl_event_loop_add_fd(xbridge->server->loop,
				process->pidfd, WL_EVENT_READABLE, handle_process_exit, process);
		}
		wl_list_insert(&xbridge->processes, &process->link);
		wlr_log(WLR_INFO, "xbridge: %s (%d) talks X%s", client->comm, (int)client->pid,
			process->claimed ? ", its windows are bridged" : "");
	}
	process->connections++;
}

// its windows go back to being plain windows, shown if they're mapped
static void release(struct flame_xbridge_process *process) {
	struct flame_xbridge_window *window, *tmp;
	wl_list_for_each_safe(window, tmp, &process->windows, link) {
		xbridge_window_detach(window);
		window->changed(window);
	}
	for (size_t i = 0; i < process->verdicts_len; i++) {
		free(process->verdicts[i].title);
	}
	free(process->verdicts);
	free(process->views);
	if (process->exit_source) {
		wl_event_source_remove(process->exit_source);
	}
	if (process->pidfd >= 0) {
		close(process->pidfd);
	}
	wl_list_remove(&process->link);
	free(process);
}

static void hook_disconnected(void *data, struct xs_client *client) {
	struct flame_xbridge *xbridge = data;
	struct flame_xbridge_process *process = find(xbridge, client->pid);
	if (!process) {
		return;
	}
	// without a pidfd there's nothing better than the last connection
	if (--process->connections <= 0 && process->pidfd < 0) {
		release(process);
	}
}

static void hook_window_created(void *data, struct xs_window *window) {
	struct flame_xbridge *xbridge = data;
	struct flame_xbridge_process *process = window->owner
		? find(xbridge, window->owner->pid) : NULL;
	// a view: something the app made inside one of its own top-levels
	if (!process || !process->claimed || !window->parent
			|| !xs_window_top_level(window->parent)) {
		return;
	}
	if (process->views_len == process->views_cap) {
		size_t cap = process->views_cap ? process->views_cap * 2 : 32;
		uint32_t *views = realloc(process->views, cap * sizeof(*views));
		if (!views) {
			wlr_log(WLR_ERROR, "out of memory");
			return;
		}
		process->views = views;
		process->views_cap = cap;
	}
	process->views[process->views_len++] = window->id;
	wlr_log(WLR_DEBUG, "xbridge: pid %d view #%zu is X window 0x%x", (int)process->pid,
		process->views_len, window->id);
}

static void hook_window_destroyed(void *data, struct xs_window *window) {
	struct flame_xbridge *xbridge = data;
	if (xs_window_top_level(window)) {
		xshell_window_destroyed(&xbridge->server->xshell, window);
	}
	struct flame_xbridge_process *process = window->owner
		? find(xbridge, window->owner->pid) : NULL;
	if (!process) {
		return;
	}
	// its number stays taken, a new window may get the same id
	for (size_t i = 0; i < process->views_len; i++) {
		if (process->views[i] == window->id) {
			process->views[i] = 0;
		}
	}
}

static void hook_windows_changed(void *data) {
	struct flame_xbridge *xbridge = data;
	struct flame_xbridge_process *process, *tmp;
	wl_list_for_each_safe(process, tmp, &xbridge->processes, link) {
		if (process->claimed) {
			refresh(process);
		}
	}
	// everyone else's top-levels are flame windows
	xshell_windows_changed(&xbridge->server->xshell);
}

// ---- what xshell does, handed over ----

static void hook_present(void *data, const struct xs_present *present) {
	struct flame_xbridge *xbridge = data;
	xshell_present(&xbridge->server->xshell, present);
}

static void hook_notify_msc(void *data, struct xs_client *client, uint32_t window,
		uint32_t serial, uint64_t target_msc, uint64_t divisor, uint64_t remainder) {
	struct flame_xbridge *xbridge = data;
	xshell_notify_msc(&xbridge->server->xshell, window, serial, target_msc, divisor, remainder);
}

static void hook_pixmap_destroyed(void *data, struct xs_pixmap *pixmap) {
	struct flame_xbridge *xbridge = data;
	xshell_pixmap_destroyed(&xbridge->server->xshell, pixmap);
}

static void hook_syncobj_destroyed(void *data, struct xs_syncobj *syncobj) {
	struct flame_xbridge *xbridge = data;
	xshell_syncobj_destroyed(&xbridge->server->xshell, syncobj);
}

static int hook_open_device(void *data) {
	struct flame_xbridge *xbridge = data;
	return xshell_open_device(&xbridge->server->xshell);
}

static size_t hook_modifiers(void *data, uint32_t format, uint64_t *out, size_t max) {
	struct flame_xbridge *xbridge = data;
	return xshell_modifiers(&xbridge->server->xshell, format, out, max);
}

static void hook_damage(void *data, struct xs_window *top, const pixman_region32_t *region) {
	struct flame_xbridge *xbridge = data;
	xshell_damaged(&xbridge->server->xshell, top, region);
}


static void hook_selection_owner(void *data, uint32_t selection, bool x_owned) {
	struct flame_xbridge *xbridge = data;
	xselection_owner_changed(&xbridge->server->xselection, selection, x_owned);
}

static void hook_selection_request(void *data, struct xs_selection_request *request) {
	struct flame_xbridge *xbridge = data;
	xselection_request(&xbridge->server->xselection, request);
}

static void hook_client_message(void *data, uint32_t window, uint32_t type,
		const uint32_t message[5]) {
	struct flame_xbridge *xbridge = data;
	xselection_client_message(&xbridge->server->xselection, window, type, message);
}

static void hook_focus_changed(void *data, uint32_t focus) {
	focus_moved(data, focus);
}

static void hook_pointer(void *data, double *x, double *y) {
	struct flame_xbridge *xbridge = data;
	struct wlr_cursor *cursor = xbridge->server->input.cursor.wlr;
	*x = cursor->x;
	*y = cursor->y;
}

// X sees flame's outputs where the layout has them, in its coordinates. the
// one at the origin is primary, else the first
static void update_screen(struct flame_xbridge *xbridge) {
	struct wlr_output_layout *layout = xbridge->server->outputs.layout;
	struct xs_monitor monitors[16];
	size_t len = 0;
	struct wlr_output_layout_output *lo;
	wl_list_for_each(lo, &layout->outputs, link) {
		struct wlr_box box;
		wlr_output_layout_get_box(layout, lo->output, &box);
		if (wlr_box_empty(&box) || len == LENGTH(monitors)) {
			continue;
		}
		struct xs_monitor *m = &monitors[len++];
		*m = (struct xs_monitor){
			.x = box.x,
			.y = box.y,
			.width = box.width,
			.height = box.height,
			.mm_width = lo->output->phys_width,
			.mm_height = lo->output->phys_height,
			.refresh = lo->output->refresh,
			.primary = box.x == 0 && box.y == 0,
		};
		snprintf(m->name, sizeof(m->name), "%s", lo->output->name);
	}
	bool primary = false;
	for (size_t i = 0; i < len; i++) {
		monitors[i].primary = monitors[i].primary && !primary;
		primary = primary || monitors[i].primary;
	}
	if (len > 0 && !primary) {
		monitors[0].primary = true;
	}
	if (len > 0) {
		xserver_set_monitors(&xbridge->xserver, monitors, len);
	}
}

static void handle_layout_change(struct wl_listener *listener, void *data) {
	struct flame_xbridge *xbridge = wl_container_of(listener, xbridge, layout_change);
	update_screen(xbridge);
}

// ---- setup ----

bool xbridge_init(struct flame_xbridge *xbridge, struct flame_server *server) {
	xbridge->server = server;
	wl_list_init(&xbridge->processes);
	struct xs_hooks hooks = {
		.connected = hook_connected,
		.disconnected = hook_disconnected,
		.window_created = hook_window_created,
		.window_destroyed = hook_window_destroyed,
		.windows_changed = hook_windows_changed,
		.focus_changed = hook_focus_changed,
		.selection_owner = hook_selection_owner,
		.selection_request = hook_selection_request,
		.client_message = hook_client_message,
		.pointer = hook_pointer,
		.open_device = hook_open_device,
		.modifiers = hook_modifiers,
		.present = hook_present,
		.notify_msc = hook_notify_msc,
		.pixmap_destroyed = hook_pixmap_destroyed,
		.syncobj_destroyed = hook_syncobj_destroyed,
		.damage = hook_damage,
		.data = xbridge,
	};
	if (!xserver_init(&xbridge->xserver, server->loop, hooks, 0, 0)) {
		wlr_log(WLR_ERROR, "failed to set up the X server");
		return false;
	}
	listener_connect(&xbridge->layout_change, &server->outputs.layout->events.change,
		handle_layout_change);
	listener_connect(&xbridge->desktop_changed, &server->desktop.events.changed,
		handle_desktop_changed);
	return true;
}

void xbridge_start(struct flame_xbridge *xbridge) {
	if (!xserver_start(&xbridge->xserver)) {
		return;
	}
	xbridge->running = true;
	update_screen(xbridge);
	char display[16];
	snprintf(display, sizeof(display), ":%d", xbridge->xserver.display);
	// not $DISPLAY: an app picking X over wayland here would draw into
	// nothing. launchers that know (tools/steam) hand it over on purpose
	setenv("FLAMEWM_X11_DISPLAY", display, true);
}

void xbridge_finish(struct flame_xbridge *xbridge) {
	listener_disconnect(&xbridge->layout_change);
	listener_disconnect(&xbridge->desktop_changed);
	xserver_finish(&xbridge->xserver);
	struct flame_xbridge_process *process, *tmp;
	wl_list_for_each_safe(process, tmp, &xbridge->processes, link) {
		release(process);
	}
}
