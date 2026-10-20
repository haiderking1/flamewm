#include "output/output.h"

#include <stdlib.h>
#include <time.h>
#include <wlr/backend.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#include "config/config.h"
#include "core/server.h"
#include "util/listener.h"

static void handle_frame(struct wl_listener *listener, void *data) {
	struct flame_output *output = wl_container_of(listener, output, frame);

	wlr_scene_output_commit(output->scene_output, NULL);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(output->scene_output, &now);
	if (transaction_frame_done(&output->server->desktop.transaction, &now)) {
		wlr_output_schedule_frame(output->wlr);
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

	listener_disconnect(&output->frame);
	listener_disconnect(&output->request_state);
	listener_disconnect(&output->destroy);

	layers_output_gone(&output->server->layers, output);
	wlr_scene_node_destroy(&output->background->node);
	wl_list_remove(&output->link);
	free(output);
}

static bool output_enable(struct wlr_output *wlr_output) {
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);

	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode) {
		wlr_output_state_set_mode(&state, mode);
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
	if (!output_enable(wlr_output)) {
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
	wlr_output->data = output;
	output->background = wlr_scene_rect_create(
		server->render.layers[FLAME_LAYER_BACKGROUND], 0, 0,
		server->config->background_color);

	listener_connect(&output->frame, &wlr_output->events.frame, handle_frame);
	listener_connect(&output->request_state, &wlr_output->events.request_state,
		handle_request_state);
	listener_connect(&output->destroy, &wlr_output->events.destroy, handle_destroy);

	wl_list_insert(outputs->list.prev, &output->link);

	// this also advertises the wl_output global to clients
	struct wlr_output_layout_output *layout_output =
		wlr_output_layout_add_auto(outputs->layout, wlr_output);
	output->scene_output = wlr_scene_output_create(server->render.scene, wlr_output);
	if (layout_output && output->scene_output) {
		wlr_scene_output_layout_add_output(server->render.scene_layout,
			layout_output, output->scene_output);
	}

	wlr_log(WLR_INFO, "output %s up: %dx%d@%.3fHz scale %.2f", wlr_output->name,
		wlr_output->width, wlr_output->height, wlr_output->refresh / 1000.0, wlr_output->scale);
}

static void handle_layout_change(struct wl_listener *listener, void *data) {
	struct flame_outputs *outputs = wl_container_of(listener, outputs, layout_change);

	struct flame_output *output;
	wl_list_for_each(output, &outputs->list, link) {
		wlr_output_layout_get_box(outputs->layout, output->wlr, &output->box);
		wlr_scene_node_set_position(&output->background->node,
			output->box.x, output->box.y);
		wlr_scene_rect_set_size(output->background,
			output->box.width, output->box.height);
		layers_arrange(&outputs->server->layers, output);
	}

	desktop_arrange(&outputs->server->desktop);
}

bool outputs_init(struct flame_outputs *outputs, struct flame_server *server) {
	outputs->server = server;
	wl_list_init(&outputs->list);

	outputs->layout = wlr_output_layout_create(server->display);
	if (!outputs->layout) {
		wlr_log(WLR_ERROR, "failed to create output layout");
		return false;
	}

	listener_connect(&outputs->new_output, &server->backend.wlr->events.new_output,
		handle_new_output);
	listener_connect(&outputs->layout_change, &outputs->layout->events.change,
		handle_layout_change);
	return true;
}

void outputs_finish(struct flame_outputs *outputs) {
	listener_disconnect(&outputs->new_output);
	listener_disconnect(&outputs->layout_change);
}

void outputs_schedule_frame(struct flame_outputs *outputs) {
	struct flame_output *output;
	wl_list_for_each(output, &outputs->list, link) {
		wlr_output_schedule_frame(output->wlr);
	}
}

struct flame_output *outputs_primary(struct flame_outputs *outputs) {
	if (wl_list_empty(&outputs->list)) {
		return NULL;
	}
	struct flame_output *output = wl_container_of(outputs->list.next, output, link);
	return output;
}
