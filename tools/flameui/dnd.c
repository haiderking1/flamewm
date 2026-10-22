#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "internal.h"

// drag and drop, wayland's (wl_data_device): files dragged out of the app
// as a uri list, and files dropped onto it, from anywhere, the app itself
// too (it goes through flame like any other). what's dropped is read down
// a pipe the window's loop watches, never waited on: the app may be the one
// writing it

#define URI_LIST "text/uri-list"
#define PLAIN "text/plain;charset=utf-8"

// what an offer has, kept on it
struct offer {
	bool uri_list;
	uint32_t source_actions, action;
};

static struct offer *offer_of(struct wl_data_offer *offer) {
	return offer ? wl_data_offer_get_user_data(offer) : NULL;
}

static void offer_drop(struct wl_data_offer *offer) {
	if (offer) {
		free(offer_of(offer));
		wl_data_offer_destroy(offer);
	}
}

static void offer_offer(void *data, struct wl_data_offer *offer, const char *mime) {
	struct offer *o = data;
	o->uri_list |= !strcmp(mime, URI_LIST);
}

static void offer_source_actions(void *data, struct wl_data_offer *offer, uint32_t actions) {
	((struct offer *)data)->source_actions = actions;
}

static void offer_action(void *data, struct wl_data_offer *offer, uint32_t action) {
	((struct offer *)data)->action = action;
}

static const struct wl_data_offer_listener offer_listener = {
	.offer = offer_offer,
	.source_actions = offer_source_actions,
	.action = offer_action,
};

// ---- dropping in ----

static void device_data_offer(void *data, struct wl_data_device *device,
		struct wl_data_offer *offer) {
	struct offer *o = calloc(1, sizeof(*o));
	wl_data_offer_add_listener(offer, &offer_listener, o);
}

static uint32_t wl_action(enum fui_dnd_action action) {
	return action == FUI_DND_MOVE ? WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE
		: action == FUI_DND_COPY ? WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY
		: WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
}

// what a drop would do there, told to the source (through flame)
static void tell(struct fui *ui, enum fui_dnd_action action) {
	struct offer *o = offer_of(ui->dnd_offer);
	if (!o) {
		return;
	}
	bool can = action != FUI_DND_NONE && o->uri_list;
	wl_data_offer_accept(ui->dnd_offer, ui->dnd_serial, can ? URI_LIST : NULL);
	uint32_t a = can ? wl_action(action) : 0;
	// both offered, ours preferred: the source decides from what it allows
	wl_data_offer_set_actions(ui->dnd_offer, can ? WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY
		| WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE : 0, a);
	ui->dnd_told = action;
}

static void peek_start(struct fui *ui);
static void peek_stop(struct fui *ui);

static void device_enter(void *data, struct wl_data_device *device, uint32_t serial,
		struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *offer) {
	struct fui *ui = data;
	if (ui->dnd_offer && ui->dnd_offer != offer) {
		offer_drop(ui->dnd_offer);
	}
	ui->dnd_offer = offer;
	ui->dnd_serial = serial;
	ui->dnd_x = wl_fixed_to_double(x);
	ui->dnd_y = wl_fixed_to_double(y);
	ui->dnd_told = (enum fui_dnd_action)-1;
	tell(ui, FUI_DND_NONE); // till the app says, next frame
	peek_start(ui);
	fui_redraw(ui);
}

static void device_leave(void *data, struct wl_data_device *device) {
	struct fui *ui = data;
	peek_stop(ui);
	offer_drop(ui->dnd_offer);
	ui->dnd_offer = NULL;
	fui_redraw(ui);
}

static void device_motion(void *data, struct wl_data_device *device, uint32_t time,
		wl_fixed_t x, wl_fixed_t y) {
	struct fui *ui = data;
	ui->dnd_x = wl_fixed_to_double(x);
	ui->dnd_y = wl_fixed_to_double(y);
	fui_redraw(ui);
}

static void drop_read(struct fui *ui, int fd, void *data);

static void device_drop(void *data, struct wl_data_device *device) {
	struct fui *ui = data;
	struct offer *o = offer_of(ui->dnd_offer);
	int pipes[2];
	if (!o || ui->dnd_told == FUI_DND_NONE || ui->drop_offer || pipe(pipes) < 0) {
		offer_drop(ui->dnd_offer);
		ui->dnd_offer = NULL;
		return;
	}
	// read once it's all written: the writer may be this very app
	wl_data_offer_receive(ui->dnd_offer, URI_LIST, pipes[1]);
	close(pipes[1]);
	wl_display_flush(ui->display);
	fcntl(pipes[0], F_SETFL, O_NONBLOCK);
	fcntl(pipes[0], F_SETFD, FD_CLOEXEC);
	ui->drop_offer = ui->dnd_offer;
	ui->drop_action = o->action == WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE ? FUI_DND_MOVE
		: FUI_DND_COPY;
	ui->drop_x = ui->dnd_x;
	ui->drop_y = ui->dnd_y;
	ui->drop_len = 0;
	ui->dnd_offer = NULL;
	peek_stop(ui);
	if (!fui_watch(ui, pipes[0], drop_read, NULL)) {
		close(pipes[0]);
		offer_drop(ui->drop_offer);
		ui->drop_offer = NULL;
	}
	fui_redraw(ui);
}

// "file:///a%20b" to "/a b", NULL for what isn't a local file
static char *path_of_uri(const char *uri, size_t len) {
	if (len >= 16 && !strncmp(uri, "file://localhost/", 17)) {
		uri += 16;
		len -= 16;
	} else if (len >= 8 && !strncmp(uri, "file:///", 8)) {
		uri += 7;
		len -= 7;
	} else {
		return NULL;
	}
	char *path = malloc(len + 1);
	size_t n = 0;
	for (size_t i = 0; path && i < len; i++) {
		unsigned v;
		if (uri[i] == '%' && i + 2 < len && sscanf(uri + i + 1, "%2x", &v) == 1) {
			path[n++] = (char)v;
			i += 2;
		} else {
			path[n++] = uri[i];
		}
	}
	if (path) {
		path[n] = '\0';
	}
	return path;
}

// a uri list's local files' paths, appended to *list
static void parse_uris(char *text, char ***list, size_t *len) {
	for (char *line = text; line && *line;) {
		size_t n = strcspn(line, "\r\n");
		char *path = line[0] != '#' ? path_of_uri(line, n) : NULL;
		char **grown = path ? realloc(*list, (*len + 1) * sizeof(char *)) : NULL;
		if (grown) {
			*list = grown;
			(*list)[(*len)++] = path;
		} else {
			free(path);
		}
		line += n;
		line += strspn(line, "\r\n");
	}
}

static void free_list(char ***list, size_t *len) {
	for (size_t i = 0; i < *len; i++) {
		free((*list)[i]);
	}
	free(*list);
	*list = NULL;
	*len = 0;
}

// fd read into *buf, false while there's more to come
static bool read_all(int fd, char **buf, size_t *len) {
	char chunk[4096];
	ssize_t n;
	while ((n = read(fd, chunk, sizeof(chunk))) > 0) {
		char *grown = realloc(*buf, *len + (size_t)n + 1);
		if (!grown) {
			break;
		}
		*buf = grown;
		memcpy(*buf + *len, chunk, (size_t)n);
		*len += (size_t)n;
	}
	if (n < 0 && errno == EAGAIN) {
		return false;
	}
	if (*buf) {
		(*buf)[*len] = '\0';
	}
	return true;
}

static void drop_read(struct fui *ui, int fd, void *data) {
	if (!read_all(fd, &ui->drop_buf, &ui->drop_len)) {
		return;
	}
	fui_unwatch(ui, fd);
	close(fd);
	parse_uris(ui->drop_buf, &ui->dropped, &ui->dropped_len);
	free(ui->drop_buf);
	ui->drop_buf = NULL;
	ui->drop_len = 0;
	// done with it: the source hears how it went (a move: it may tidy up)
	wl_data_offer_finish(ui->drop_offer);
	offer_drop(ui->drop_offer);
	ui->drop_offer = NULL;
	ui->dropped_ready = ui->dropped_len > 0;
	fui_redraw(ui);
}

// what's dragged over, read while it's over (the app picks a move or a
// copy by where they're from)
static void peek_read(struct fui *ui, int fd, void *data) {
	if (!read_all(fd, &ui->peek_buf, &ui->peek_len)) {
		return;
	}
	fui_unwatch(ui, fd);
	close(fd);
	ui->peek_fd = -1;
	parse_uris(ui->peek_buf, &ui->peeked, &ui->peeked_len);
	free(ui->peek_buf);
	ui->peek_buf = NULL;
	ui->peek_len = 0;
	ui->peeked_ready = true;
	fui_redraw(ui);
}

static void peek_stop(struct fui *ui) {
	if (ui->peek_fd >= 0) {
		fui_unwatch(ui, ui->peek_fd);
		close(ui->peek_fd);
		ui->peek_fd = -1;
	}
	free(ui->peek_buf);
	ui->peek_buf = NULL;
	ui->peek_len = 0;
	free_list(&ui->peeked, &ui->peeked_len);
	ui->peeked_ready = false;
}

static void peek_start(struct fui *ui) {
	peek_stop(ui);
	struct offer *o = offer_of(ui->dnd_offer);
	int pipes[2];
	if (!o || !o->uri_list || pipe(pipes) < 0) {
		return;
	}
	wl_data_offer_receive(ui->dnd_offer, URI_LIST, pipes[1]);
	close(pipes[1]);
	wl_display_flush(ui->display);
	fcntl(pipes[0], F_SETFL, O_NONBLOCK);
	fcntl(pipes[0], F_SETFD, FD_CLOEXEC);
	if (fui_watch(ui, pipes[0], peek_read, NULL)) {
		ui->peek_fd = pipes[0];
	} else {
		close(pipes[0]);
	}
}

static void device_selection(void *data, struct wl_data_device *device,
		struct wl_data_offer *offer) {
	struct fui *ui = data;
	if (offer && offer != ui->dnd_offer) {
		offer_drop(offer); // the clipboard: not ours to read (yet)
	}
}

static const struct wl_data_device_listener device_listener = {
	.data_offer = device_data_offer,
	.enter = device_enter,
	.leave = device_leave,
	.motion = device_motion,
	.drop = device_drop,
	.selection = device_selection,
};

bool fui_dnd_over(struct fui *ui, double *x, double *y) {
	struct offer *o = offer_of(ui->dnd_offer);
	if (!o || !o->uri_list) {
		return false;
	}
	*x = ui->dnd_x;
	*y = ui->dnd_y;
	return true;
}

char **fui_dnd_files(struct fui *ui, size_t *len) {
	if (!ui->dnd_offer || !ui->peeked_ready) {
		return NULL;
	}
	*len = ui->peeked_len;
	return ui->peeked;
}

void fui_dnd_accept(struct fui *ui, enum fui_dnd_action action) {
	if (ui->dnd_offer && action != ui->dnd_told) {
		tell(ui, action);
	}
}

char **fui_dnd_dropped(struct fui *ui, size_t *len, enum fui_dnd_action *action, double *x,
		double *y) {
	if (!ui->dropped_ready) {
		return NULL;
	}
	*len = ui->dropped_len;
	*action = ui->drop_action;
	*x = ui->drop_x;
	*y = ui->drop_y;
	return ui->dropped;
}

// ---- dragging out ----

struct writing {
	int fd;
	char *data;
};

// a source's data written on a thread of its own: the reader may be us
static void *write_out(void *data) {
	struct writing *w = data;
	size_t len = strlen(w->data);
	for (size_t off = 0; off < len;) {
		ssize_t n = write(w->fd, w->data + off, len - off);
		if (n <= 0) {
			if (n < 0 && errno == EINTR) {
				continue;
			}
			break;
		}
		off += (size_t)n;
	}
	close(w->fd);
	free(w->data);
	free(w);
	return NULL;
}

static void drag_end(struct fui *ui) {
	if (ui->drag_source) {
		wl_data_source_destroy(ui->drag_source);
		ui->drag_source = NULL;
	}
	if (ui->drag_icon) {
		wl_surface_destroy(ui->drag_icon);
		ui->drag_icon = NULL;
	}
	fui_buffer_free(&ui->drag_buffer);
	free(ui->drag_uris);
	free(ui->drag_paths);
	ui->drag_uris = ui->drag_paths = NULL;
	fui_redraw(ui);
}

static void source_target(void *data, struct wl_data_source *source, const char *mime) {
}

static void source_send(void *data, struct wl_data_source *source, const char *mime, int fd) {
	struct fui *ui = data;
	const char *what = !strcmp(mime, URI_LIST) ? ui->drag_uris : ui->drag_paths;
	struct writing *w = malloc(sizeof(*w));
	pthread_t thread;
	pthread_attr_t attr;
	if (!what || !w || !(w->data = strdup(what))) {
		free(w);
		close(fd);
		return;
	}
	w->fd = fd;
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&thread, &attr, write_out, w)) {
		close(fd);
		free(w->data);
		free(w);
	}
	pthread_attr_destroy(&attr);
}

static void source_cancelled(void *data, struct wl_data_source *source) {
	drag_end(data);
}

static void source_drop_performed(void *data, struct wl_data_source *source) {
}

static void source_finished(void *data, struct wl_data_source *source) {
	drag_end(data);
}

static void source_action(void *data, struct wl_data_source *source, uint32_t action) {
}

static const struct wl_data_source_listener source_listener = {
	.target = source_target,
	.send = source_send,
	.cancelled = source_cancelled,
	.dnd_drop_performed = source_drop_performed,
	.dnd_finished = source_finished,
	.action = source_action,
};

// path as a file uri: everything but the plain bytes escaped
static void append_uri(char **out, size_t *len, const char *path) {
	size_t need = *len + 7 + strlen(path) * 3 + 3;
	char *grown = realloc(*out, need);
	if (!grown) {
		return;
	}
	*out = grown;
	*len += (size_t)sprintf(*out + *len, "file://");
	for (const unsigned char *c = (const unsigned char *)path; *c; c++) {
		if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9')
				|| strchr("!$&'()*+,-./:=@_~", *c)) {
			(*out)[(*len)++] = (char)*c;
		} else {
			*len += (size_t)sprintf(*out + *len, "%%%02X", *c);
		}
	}
	*len += (size_t)sprintf(*out + *len, "\r\n");
}

bool fui_drag_files(struct fui *ui, char *const *paths, size_t len, int icon_width,
		int icon_height, void (*draw_icon)(struct fui *ui, void *data), void *data) {
	if (!ui->data_device || ui->drag_source || !len || !ui->button_down) {
		return false;
	}
	size_t ul = 0, pl = 0;
	for (size_t i = 0; i < len; i++) {
		append_uri(&ui->drag_uris, &ul, paths[i]);
		char *grown = realloc(ui->drag_paths, pl + strlen(paths[i]) + 2);
		if (grown) {
			ui->drag_paths = grown;
			pl += (size_t)sprintf(ui->drag_paths + pl, "%s%s", pl ? "\n" : "", paths[i]);
		}
	}
	if (!ui->drag_uris || !ui->drag_paths) {
		drag_end(ui);
		return false;
	}
	ui->drag_source = wl_data_device_manager_create_data_source(ui->data_manager);
	wl_data_source_add_listener(ui->drag_source, &source_listener, ui);
	wl_data_source_offer(ui->drag_source, URI_LIST);
	wl_data_source_offer(ui->drag_source, PLAIN);
	wl_data_source_set_actions(ui->drag_source, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY
		| WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
	// the icon under the pointer, drawn by the app at the screen's scale
	int scale = (int)ceil(ui->scale120 / 120.0);
	ui->drag_icon = wl_compositor_create_surface(ui->compositor);
	if (draw_icon && fui_buffer_make(ui, &ui->drag_buffer, icon_width * scale,
			icon_height * scale)) {
		cairo_surface_t *target = cairo_image_surface_create_for_data(ui->drag_buffer.data,
			CAIRO_FORMAT_ARGB32, ui->drag_buffer.width, ui->drag_buffer.height,
			ui->drag_buffer.stride);
		cairo_t *cr = cairo_create(target);
		cairo_scale(cr, scale, scale);
		// the app's drawing calls draw here for now
		cairo_t *was = ui->cr;
		ui->cr = cr;
		draw_icon(ui, data);
		ui->cr = was;
		cairo_destroy(cr);
		cairo_surface_flush(target);
		cairo_surface_destroy(target);
	}
	wl_data_device_start_drag(ui->data_device, ui->drag_source, ui->surface, ui->drag_icon,
		ui->press_serial);
	if (ui->drag_buffer.buffer) {
		wl_surface_set_buffer_scale(ui->drag_icon, scale);
		// its middle under the pointer
		wl_surface_attach(ui->drag_icon, ui->drag_buffer.buffer, -icon_width / 2,
			-icon_height / 2);
		wl_surface_damage(ui->drag_icon, 0, 0, icon_width, icon_height);
	}
	wl_surface_commit(ui->drag_icon);
	// the drag has the pointer now: its release won't come here
	ui->button_down = false;
	ui->active = 0;
	return true;
}

bool fui_dragging(const struct fui *ui) {
	return ui->drag_source != NULL;
}

void fui_dnd_init(struct fui *ui) {
	ui->peek_fd = -1;
	if (ui->data_manager && ui->seat) {
		ui->data_device = wl_data_device_manager_get_data_device(ui->data_manager, ui->seat);
		wl_data_device_add_listener(ui->data_device, &device_listener, ui);
	}
}

void fui_dnd_frame_end(struct fui *ui) {
	if (!ui->dropped_ready) {
		return;
	}
	for (size_t i = 0; i < ui->dropped_len; i++) {
		free(ui->dropped[i]);
	}
	free(ui->dropped);
	ui->dropped = NULL;
	ui->dropped_len = 0;
	ui->dropped_ready = false;
}

void fui_dnd_finish(struct fui *ui) {
	ui->dropped_ready = true;
	fui_dnd_frame_end(ui);
	offer_drop(ui->dnd_offer);
	ui->dnd_offer = NULL;
	offer_drop(ui->drop_offer);
	ui->drop_offer = NULL;
	free(ui->drop_buf);
	ui->drop_buf = NULL;
	peek_stop(ui);
	drag_end(ui);
	if (ui->data_device) {
		wl_data_device_release(ui->data_device);
		ui->data_device = NULL;
	}
	if (ui->data_manager) {
		wl_data_device_manager_destroy(ui->data_manager);
		ui->data_manager = NULL;
	}
}
