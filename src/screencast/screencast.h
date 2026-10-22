#ifndef FLAME_SCREENCAST_SCREENCAST_H
#define FLAME_SCREENCAST_SCREENCAST_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>

struct flame_server;
struct flame_output;
struct flame_view;
struct cast_stream;

// screen sharing's video: a pipewire stream a screen or one window, drawn by
// flame straight into the stream's buffers (gpu buffers the app imports, or
// shared memory for the ones that can't). a frame goes out when what it
// shows changed, as fast as the app takes them, and once a second anyway.
// a window is drawn on its own, whatever covers it, on a hidden workspace
// too. the portal (portal/) hands the stream's node to the app
struct flame_screencast {
	struct flame_server *server;
	bool usable; // connected to pipewire
	struct pw_loop *loop;
	struct pw_context *context;
	struct pw_core *core;
	struct wl_event_source *loop_source;
	struct wl_list streams; // cast_stream.link
	// something drew: streams look whether what they show changed
	struct wl_listener scene_commit, scene_changed;
	struct wl_event_source *poke;
};

bool screencast_init(struct flame_screencast *screencast, struct flame_server *server);
void screencast_finish(struct flame_screencast *screencast);

// what a stream shows: an output (the pointer drawn in when cursor), or a
// window
struct cast_target {
	struct flame_output *output;
	struct flame_view *view;
	bool cursor;
};

struct cast_stream_events {
	// the stream's pipewire node is there, the app can be given it. width
	// and height: what it shows, in pixels
	void (*ready)(struct cast_stream *stream, uint32_t node, int width, int height,
		void *data);
	// it can't go on (its window closed, its screen went, pipewire went). it
	// comes from an idle, destroying the stream in it is fine
	void (*ended)(struct cast_stream *stream, void *data);
};

// NULL without pipewire or memory. ready comes once the node exists
struct cast_stream *screencast_stream_create(struct flame_screencast *screencast,
	const struct cast_target *target, const struct cast_stream_events *events, void *data);
void screencast_stream_destroy(struct cast_stream *stream);

// view is going away: streams of it end
void screencast_view_gone(struct flame_screencast *screencast, struct flame_view *view);

// a still of target (no pointer) as a png at path: the screenshot portal's.
// false when it couldn't be drawn or written
bool screencast_shoot(struct flame_screencast *screencast, const struct cast_target *target,
	const char *path);

#endif
