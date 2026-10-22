#include "output/output.h"

#include <drm_fourcc.h>
#include <stdio.h>
#include <stdlib.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/multi.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/log.h>

#include "config/config.h"
#include "core/server.h"
#include "output/mode.h"
#include "render/snapshot.h"
#include "util/listener.h"

// a mouse resize lands right before the frame that shows it, animations
// step to where they'll be when it's on screen
static bool before_render(void *data, int64_t target) {
	struct flame_output *output = data;
	drag_frame(&output->server->input.drag);
	return anims_tick(&output->server->anims, target);
}

// the workspace showing has a window fullscreen on screen. workspaces are laid
// out on the primary output, the others never have one
static bool fullscreen_shown(void *data) {
	struct flame_output *output = data;
	struct flame_desktop *desktop = &output->server->desktop;
	struct flame_view *view = desktop->active ? desktop->active->fullscreen : NULL;
	return view && view->fullscreen_shown && output == outputs_primary(&output->server->outputs);
}

// windows a transaction waits on may be hidden, they still need callbacks to
// draw what it's waiting for. this keeps them coming once a refresh: only new
// cycles count, the revisits in between would turn this into a busy loop
static void callbacks_sent(void *data, const struct timespec *now, bool new_cycle) {
	struct flame_output *output = data;
	if (new_cycle && transaction_frame_done(&output->server->desktop.transaction, now)) {
		frame_request(&output->frames);
	}
}

static void handle_request_state(struct wl_listener *listener, void *data) {
	// nested backends ask for this when their window gets resized
	struct flame_output *output = wl_container_of(listener, output, request_state);
	const struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(output->wlr, event->state);
}

static void handle_destroy(struct wl_listener *listener, void *data) {
	struct flame_output *output = wl_container_of(listener, output, destroy);

	listener_disconnect(&output->request_state);
	listener_disconnect(&output->destroy);

	layers_output_gone(&output->server->layers, output);
	scene_surfaces_output_gone(&output->server->render.surfaces, output->wlr);
	frame_finish(&output->frames);
	scene_node_destroy(&output->background->node);
	wl_list_remove(&output->link);
	free(output);
}

// the scale init.lua asks for, 1 when it says nothing
static float configured_scale(const struct flame_config *config, const char *name) {
	const struct flame_output_config *oc = config_output(config, name);
	return oc && oc->scale > 0 ? oc->scale : 1;
}

// the display's native size at its fastest refresh (output/mode.h)
static struct wlr_output_mode *best_mode(struct wlr_output *wlr_output) {
	size_t count = (size_t)wl_list_length(&wlr_output->modes);
	struct mode_info *infos = count ? calloc(count, sizeof(*infos)) : NULL;
	if (!infos) {
		return NULL;
	}
	size_t i = 0;
	struct wlr_output_mode *mode;
	wl_list_for_each(mode, &wlr_output->modes, link) {
		infos[i++] = (struct mode_info){mode->width, mode->height, mode->refresh,
			mode->preferred};
	}
	int pick = mode_pick(infos, count);
	free(infos);
	i = 0;
	wl_list_for_each(mode, &wlr_output->modes, link) {
		if ((int)i++ == pick) {
			return mode;
		}
	}
	return NULL;
}

static bool output_enable(struct wlr_output *wlr_output, const struct flame_config *config) {
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	wlr_output_state_set_scale(&state, configured_scale(config, wlr_output->name));

	// the preferred mode when the fastest one won't light up (a cable or a
	// gpu port that can't carry it)
	struct wlr_output_mode *preferred = wlr_output_preferred_mode(wlr_output);
	struct wlr_output_mode *mode = best_mode(wlr_output);
	if (mode) {
		wlr_output_state_set_mode(&state, mode);
		if (mode != preferred && preferred && !wlr_output_test_state(wlr_output, &state)) {
			wlr_log(WLR_INFO, "%s: %dx%d@%.3fHz won't light up, the preferred mode then",
				wlr_output->name, mode->width, mode->height, mode->refresh / 1000.0);
			wlr_output_state_set_mode(&state, preferred);
		}
	}
	// tests: a headless screen the size of a real one, FLAME_HEADLESS_SIZE=WxH
	const char *size = getenv("FLAME_HEADLESS_SIZE");
	int width, height;
	if (size && wlr_output_is_headless(wlr_output)
			&& sscanf(size, "%dx%d", &width, &height) == 2 && width > 0 && height > 0) {
		wlr_output_state_set_custom_mode(&state, width, height, 0);
	}

	// frames with alpha, 32 bits either way: nvidia reads an XRGB buffer
	// back as 24 bit, and a 24 bit row has to be a multiple of 3 bytes for
	// wl_shm and of 4 for pixman, so screen capture (grim) breaks at most
	// widths. snapshots keep translucent windows translucent too. a screen
	// that won't take it stays XRGB
	wlr_output_state_set_render_format(&state, DRM_FORMAT_ARGB8888);
	if (!wlr_output_test_state(wlr_output, &state)) {
		wlr_output_state_set_render_format(&state, DRM_FORMAT_XRGB8888);
	}

	bool ok = wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);
	return ok;
}

static void handle_new_output(struct wl_listener *listener, void *data) {
	struct flame_outputs *outputs = wl_container_of(listener, outputs, new_output);
	struct flame_server *server = outputs->server;
	struct wlr_output *wlr_output = data;

	if (!wlr_output_init_render(wlr_output, server->render.allocator,
			server->render.renderer)) {
		wlr_log(WLR_ERROR, "failed to init rendering on %s", wlr_output->name);
		return;
	}
	if (!output_enable(wlr_output, server->config)) {
		wlr_log(WLR_ERROR, "failed to enable %s", wlr_output->name);
		return;
	}

	struct flame_output *output = calloc(1, sizeof(*output));
	if (!output) {
		wlr_log(WLR_ERROR, "out of memory");
		return;
	}
	output->server = server;
	output->wlr = wlr_output;
	output->background = scene_rect_create(server->render.layers[FLAME_LAYER_BACKGROUND],
		0, 0, server->config->background_color);
	struct flame_frame_hooks hooks = {
		.data = output,
		.before_render = before_render,
		.callbacks_sent = callbacks_sent,
		.fullscreen = fullscreen_shown,
	};
	if (!output->background || !frame_init(&output->frames, &server->render, wlr_output,
			server->backend.session, &output->box, hooks)) {
		wlr_log(WLR_ERROR, "failed to set up rendering on %s", wlr_output->name);
		if (output->background) {
			scene_node_destroy(&output->background->node);
		}
		free(output);
		return;
	}
	wlr_output->data = output;

	listener_connect(&output->request_state, &wlr_output->events.request_state,
		handle_request_state);
	listener_connect(&output->destroy, &wlr_output->events.destroy, handle_destroy);

	wl_list_insert(outputs->list.prev, &output->link);

	// this also advertises the wl_output global to clients
	wlr_output_layout_add_auto(outputs->layout, wlr_output);

	wl_signal_emit_mutable(&outputs->events.new_frame, &output->frames);

	wlr_log(WLR_INFO, "output %s up: %dx%d@%.3fHz scale %.2f", wlr_output->name,
		wlr_output->width, wlr_output->height, wlr_output->refresh / 1000.0, wlr_output->scale);
}

static void handle_layout_change(struct wl_listener *listener, void *data) {
	struct flame_outputs *outputs = wl_container_of(listener, outputs, layout_change);

	struct flame_output *output;
	wl_list_for_each(output, &outputs->list, link) {
		wlr_output_layout_get_box(outputs->layout, output->wlr, &output->box);
		scene_node_set_position(&output->background->node, output->box.x, output->box.y);
		scene_rect_set_size(output->background, output->box.width, output->box.height);
		layers_arrange(&outputs->server->layers, output);
		render_snapshot_warm(&output->frames);
	}

	desktop_arrange(&outputs->server->desktop);
}

// a display changed under the same plug: freesync turned on or off in its
// own menu. wlroots took vrr support from the plug, so it's read again here
static void handle_hotplug(struct wl_listener *listener, void *data) {
	struct flame_outputs *outputs = wl_container_of(listener, outputs, hotplug);
	struct flame_output *output;
	wl_list_for_each(output, &outputs->list, link) {
		bool capable;
		if (!backend_vrr_capable(output->wlr, &capable)
				|| capable == output->wlr->adaptive_sync_supported) {
			continue;
		}
		wlr_log(WLR_INFO, "%s: the display %s vrr now", output->wlr->name,
			capable ? "can do" : "can't do");
		output->wlr->adaptive_sync_supported = capable;
		frame_display_changed(&output->frames);
	}
}

bool outputs_init(struct flame_outputs *outputs, struct flame_server *server) {
	outputs->server = server;
	wl_list_init(&outputs->list);
	wl_signal_init(&outputs->events.new_frame);

	outputs->layout = wlr_output_layout_create(server->display);
	if (!outputs->layout) {
		wlr_log(WLR_ERROR, "failed to create output layout");
		return false;
	}

	listener_connect(&outputs->new_output, &server->backend.wlr->events.new_output,
		handle_new_output);
	listener_connect(&outputs->layout_change, &outputs->layout->events.change,
		handle_layout_change);
	listener_connect(&outputs->hotplug, &server->backend.events.hotplug, handle_hotplug);
	return true;
}

void outputs_finish(struct flame_outputs *outputs) {
	listener_disconnect(&outputs->new_output);
	listener_disconnect(&outputs->layout_change);
	listener_disconnect(&outputs->hotplug);
}

void outputs_schedule_frame(struct flame_outputs *outputs) {
	struct flame_output *output;
	wl_list_for_each(output, &outputs->list, link) {
		frame_request(&output->frames);
	}
}

struct flame_output *outputs_primary(struct flame_outputs *outputs) {
	if (wl_list_empty(&outputs->list)) {
		return NULL;
	}
	struct flame_output *output = wl_container_of(outputs->list.next, output, link);
	return output;
}

void outputs_apply_config(struct flame_outputs *outputs) {
	struct flame_output *output;
	wl_list_for_each(output, &outputs->list, link) {
		float scale = configured_scale(outputs->server->config, output->wlr->name);
		if (scale == output->wlr->scale) {
			continue;
		}
		struct wlr_output_state state;
		wlr_output_state_init(&state);
		wlr_output_state_set_scale(&state, scale);
		if (!wlr_output_commit_state(output->wlr, &state)) {
			wlr_log(WLR_ERROR, "%s: can't change scale to %.2f", output->wlr->name, scale);
		}
		wlr_output_state_finish(&state);
	}
}

void outputs_test_unplug(struct flame_outputs *outputs) {
	struct flame_output *output, *tmp;
	wl_list_for_each_safe(output, tmp, &outputs->list, link) {
		wlr_output_destroy(output->wlr);
	}
}

static void find_headless(struct wlr_backend *backend, void *data) {
	struct wlr_backend **found = data;
	if (!*found && wlr_backend_is_headless(backend)) {
		*found = backend;
	}
}

bool outputs_test_plug(struct flame_outputs *outputs) {
	struct wlr_backend *headless = NULL;
	struct wlr_backend *backend = outputs->server->backend.wlr;
	if (wlr_backend_is_multi(backend)) {
		wlr_multi_for_each_backend(backend, find_headless, &headless);
	} else if (wlr_backend_is_headless(backend)) {
		headless = backend;
	}
	return headless && wlr_headless_add_output(headless, 1280, 720);
}
