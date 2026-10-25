#include "eyecandy/eyecandy.h"

#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/util/addon.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>

#include "core/server.h"
#include "ext-background-effect-v1-protocol.h"
#include "flame-eyecandy-v1-protocol.h"
#include "util/listener.h"

// a surface's glass, from either protocol: what's pending, what's current.
// both protocols' panes live on one record hung on the surface (an addon),
// it goes with the surface

#define MAX_PANES 32

// what ext-background-effect's blur is drawn as: plain frosted, no rim, no
// bending, the app's own pixels on top say how it looks
static const struct flame_glass_style blur_style = {
	.blur = 18,
	.saturation = 1.3f,
};

struct pane_set {
	struct eyecandy_pane panes[MAX_PANES];
	size_t len;
};

struct surface_glass {
	struct wlr_addon addon;
	struct flame_server *server;
	struct wlr_surface *surface;
	struct wl_resource *glass, *background; // the objects, NULL for none
	// flame-eyecandy's and ext-background-effect's, each pending and current
	struct pane_set candy_pending, candy, blur_pending, blur;
	bool candy_dirty, blur_dirty;
	double shape_pending, shape; // set_shape's radius, 0 for none
	bool shape_dirty;
	double opacity_pending, opacity; // all its panes faded to this, 1 for not at all
	// what the frame reads: both, candy's over blur's
	struct pane_set all;
	struct wl_listener commit;
};

static void addon_destroy(struct wlr_addon *addon);

static const struct wlr_addon_interface addon_impl = {
	.name = "flame_eyecandy",
	.destroy = addon_destroy,
};

static struct surface_glass *glass_of(struct wlr_surface *surface) {
	struct wlr_addon *addon = wlr_addon_find(&surface->addons, NULL, &addon_impl);
	struct surface_glass *glass;
	return addon ? wl_container_of(addon, glass, addon) : NULL;
}

static void rebuild(struct surface_glass *glass) {
	glass->all.len = 0;
	const struct pane_set *sets[2] = {&glass->blur, &glass->candy};
	for (int s = 0; s < 2; s++) {
		for (size_t i = 0; i < sets[s]->len && glass->all.len < MAX_PANES; i++) {
			glass->all.panes[glass->all.len++] = sets[s]->panes[i];
		}
	}
}

static void handle_arrange(void *data) {
	struct flame_server *server = data;
	desktop_arrange(&server->desktop);
}

static void handle_commit(struct wl_listener *listener, void *data) {
	struct surface_glass *glass = wl_container_of(listener, glass, commit);
	glass->opacity = glass->opacity_pending;
	if (glass->shape_dirty) {
		glass->shape_dirty = false;
		if (glass->shape != glass->shape_pending) {
			glass->shape = glass->shape_pending;
			// its border comes or goes: the layout's done again, after this
			// commit's through
			wl_event_loop_add_idle(glass->server->loop, handle_arrange, glass->server);
		}
	}
	if (!glass->candy_dirty && !glass->blur_dirty) {
		return;
	}
	if (glass->candy_dirty) {
		glass->candy = glass->candy_pending;
	}
	if (glass->blur_dirty) {
		glass->blur = glass->blur_pending;
	}
	glass->candy_dirty = glass->blur_dirty = false;
	rebuild(glass);
}

static void glass_free(struct surface_glass *glass) {
	listener_disconnect(&glass->commit);
	wlr_addon_finish(&glass->addon);
	free(glass);
}

// both objects gone: nothing to keep around
static void maybe_free(struct surface_glass *glass) {
	if (!glass->glass && !glass->background && !glass->all.len && !glass->candy_dirty
			&& !glass->blur_dirty && !glass->shape && !glass->shape_dirty) {
		glass_free(glass);
	}
}

static void addon_destroy(struct wlr_addon *addon) {
	// the surface goes: its objects go inert
	struct surface_glass *glass = wl_container_of(addon, glass, addon);
	if (glass->glass) {
		wl_resource_set_user_data(glass->glass, NULL);
	}
	if (glass->background) {
		wl_resource_set_user_data(glass->background, NULL);
	}
	glass_free(glass);
}

static struct surface_glass *glass_get(struct flame_server *server,
		struct wlr_surface *surface) {
	struct surface_glass *glass = glass_of(surface);
	if (glass) {
		return glass;
	}
	glass = calloc(1, sizeof(*glass));
	if (!glass) {
		return NULL;
	}
	glass->server = server;
	glass->surface = surface;
	glass->opacity = glass->opacity_pending = 1;
	wlr_addon_init(&glass->addon, &surface->addons, NULL, &addon_impl);
	listener_connect(&glass->commit, &surface->events.commit, handle_commit);
	return glass;
}

bool eyecandy_shaped(struct wlr_surface *surface) {
	struct surface_glass *glass = glass_of(surface);
	return glass && glass->shape > 0;
}

double eyecandy_opacity(struct wlr_surface *surface) {
	struct surface_glass *glass = glass_of(surface);
	return glass ? glass->opacity : 1;
}

double eyecandy_shape(struct wlr_surface *surface) {
	struct surface_glass *glass = glass_of(surface);
	return glass ? glass->shape : 0;
}

const struct eyecandy_pane *eyecandy_panes(struct wlr_surface *surface, size_t *len) {
	struct surface_glass *glass = glass_of(surface);
	*len = glass ? glass->all.len : 0;
	if (!*len) {
		return NULL;
	}
	// an app's own blur reaches as far as the config says now, not as it
	// said when the app last committed. blur off: no pane blurs. the login
	// screen's look is flamegreet's to say (each person's own), not a config's
	const struct flame_config *config = glass->server->config;
	bool blur = config->blur.enabled || greeter_active(&glass->server->greeter);
	for (size_t i = 0; i < *len; i++) {
		struct eyecandy_pane *pane = &glass->all.panes[i];
		pane->style.blur = !blur ? 0 : pane->app_blur ? config->blur.size : pane->asked_blur;
	}
	return glass->all.panes;
}

// ---- flame-eyecandy-v1 ----

static float clampf(float v, float lo, float hi) {
	return v < lo ? lo : v > hi ? hi : v;
}

static void glass_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void glass_handle_clear(struct wl_client *client, struct wl_resource *resource) {
	struct surface_glass *glass = wl_resource_get_user_data(resource);
	if (glass) {
		glass->candy_pending.len = 0;
		glass->candy_dirty = true;
	}
}

static void glass_handle_add_pane(struct wl_client *client, struct wl_resource *resource,
		int32_t x, int32_t y, int32_t width, int32_t height, wl_fixed_t radius,
		wl_fixed_t blur, uint32_t tint, wl_fixed_t saturation, wl_fixed_t edge,
		wl_fixed_t refraction) {
	struct surface_glass *glass = wl_resource_get_user_data(resource);
	if (!glass || width <= 0 || height <= 0 || glass->candy_pending.len >= MAX_PANES) {
		return;
	}
	// straight alpha in, premultiplied in the scene
	float a = (float)(tint & 0xff) / 255.0f;
	struct eyecandy_pane *pane = &glass->candy_pending.panes[glass->candy_pending.len++];
	*pane = (struct eyecandy_pane){
		.box = {x, y, width, height},
		.style = {
			.radius = clampf((float)wl_fixed_to_double(radius), 0, 4096),
			// what flame's blur reaches, an app can't make every frame cost more
			.blur = clampf((float)wl_fixed_to_double(blur), 0, 64),
			.tint = {
				(float)(tint >> 24 & 0xff) / 255.0f * a,
				(float)(tint >> 16 & 0xff) / 255.0f * a,
				(float)(tint >> 8 & 0xff) / 255.0f * a,
				a,
			},
			.saturation = clampf((float)wl_fixed_to_double(saturation), 0, 3),
			.edge = clampf((float)wl_fixed_to_double(edge), 0, 2),
			.refraction = clampf((float)wl_fixed_to_double(refraction), 0, 32),
		},
		.opacity = 1,
	};
	pane->asked_blur = pane->style.blur;
	glass->candy_dirty = true;
}

static void glass_handle_set_pane_opacity(struct wl_client *client,
		struct wl_resource *resource, wl_fixed_t opacity) {
	struct surface_glass *glass = wl_resource_get_user_data(resource);
	if (glass && glass->candy_pending.len) {
		glass->candy_pending.panes[glass->candy_pending.len - 1].opacity =
			clampf((float)wl_fixed_to_double(opacity), 0, 1);
		glass->candy_dirty = true;
	}
}

static void glass_handle_set_shape(struct wl_client *client, struct wl_resource *resource,
		wl_fixed_t radius) {
	struct surface_glass *glass = wl_resource_get_user_data(resource);
	if (glass) {
		double r = wl_fixed_to_double(radius);
		glass->shape_pending = r > 0 ? (r < 4096 ? r : 4096) : 0;
		glass->shape_dirty = true;
	}
}

static void glass_handle_set_opacity(struct wl_client *client, struct wl_resource *resource,
		wl_fixed_t opacity) {
	struct surface_glass *glass = wl_resource_get_user_data(resource);
	if (glass) {
		glass->opacity_pending = clampf((float)wl_fixed_to_double(opacity), 0, 1);
	}
}

static const struct flame_glass_v1_interface glass_impl = {
	.destroy = glass_handle_destroy,
	.clear = glass_handle_clear,
	.set_shape = glass_handle_set_shape,
	.set_opacity = glass_handle_set_opacity,
	.add_pane = glass_handle_add_pane,
	.set_pane_opacity = glass_handle_set_pane_opacity,
};

static void glass_resource_destroy(struct wl_resource *resource) {
	struct surface_glass *glass = wl_resource_get_user_data(resource);
	if (!glass) {
		return;
	}
	// its panes go with the next commit, its shape too
	glass->glass = NULL;
	glass->candy_pending.len = 0;
	glass->candy_dirty = true;
	glass->shape_pending = 0;
	glass->shape_dirty = true;
	maybe_free(glass);
}

static void manager_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void manager_handle_get_glass(struct wl_client *client, struct wl_resource *resource,
		uint32_t id, struct wl_resource *surface_resource) {
	struct flame_eyecandy *eyecandy = wl_resource_get_user_data(resource);
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
	struct surface_glass *glass = glass_get(eyecandy->server, surface);
	if (glass && glass->glass) {
		wl_resource_post_error(resource, FLAME_EYECANDY_MANAGER_V1_ERROR_GLASS_EXISTS,
			"the surface has glass already");
		return;
	}
	struct wl_resource *made = wl_resource_create(client, &flame_glass_v1_interface,
		wl_resource_get_version(resource), id);
	if (!made || !glass) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(made, &glass_impl, glass, glass_resource_destroy);
	glass->glass = made;
}

static const struct flame_eyecandy_manager_v1_interface manager_impl = {
	.destroy = manager_handle_destroy,
	.get_glass = manager_handle_get_glass,
};

static void bind_eyecandy(struct wl_client *client, void *data, uint32_t version,
		uint32_t id) {
	struct wl_resource *resource = wl_resource_create(client,
		&flame_eyecandy_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &manager_impl, data, NULL);
}

// ---- ext-background-effect-v1 ----

static void background_handle_destroy(struct wl_client *client, struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static int span(int32_t from, int32_t to) {
	int64_t len = (int64_t)to - from;
	return len < INT32_MAX ? (int)len : INT32_MAX;
}

static void background_handle_set_blur_region(struct wl_client *client,
		struct wl_resource *resource, struct wl_resource *region_resource) {
	struct surface_glass *glass = wl_resource_get_user_data(resource);
	if (!glass) {
		wl_resource_post_error(resource, EXT_BACKGROUND_EFFECT_SURFACE_V1_ERROR_SURFACE_DESTROYED,
			"the surface is gone");
		return;
	}
	glass->blur_pending.len = 0;
	glass->blur_dirty = true;
	if (!region_resource) {
		return;
	}
	// its rectangles, each a plain pane
	const pixman_region32_t *region = wlr_region_from_resource(region_resource);
	int n;
	const pixman_box32_t *rects = pixman_region32_rectangles(
		(pixman_region32_t *)region, &n);
	for (int i = 0; i < n && glass->blur_pending.len < MAX_PANES; i++) {
		glass->blur_pending.panes[glass->blur_pending.len++] = (struct eyecandy_pane){
			// a region can span more than an int's worth, it's cut to the
			// surface when drawn anyway
			.box = {rects[i].x1, rects[i].y1, span(rects[i].x1, rects[i].x2),
				span(rects[i].y1, rects[i].y2)},
			.style = blur_style,
			.opacity = 1,
			.app_blur = true,
		};
	}
}

static const struct ext_background_effect_surface_v1_interface background_impl = {
	.destroy = background_handle_destroy,
	.set_blur_region = background_handle_set_blur_region,
};

static void background_resource_destroy(struct wl_resource *resource) {
	struct surface_glass *glass = wl_resource_get_user_data(resource);
	if (!glass) {
		return;
	}
	glass->background = NULL;
	glass->blur_pending.len = 0;
	glass->blur_dirty = true;
	maybe_free(glass);
}

static void background_manager_handle_destroy(struct wl_client *client,
		struct wl_resource *resource) {
	wl_resource_destroy(resource);
}

static void background_manager_handle_get(struct wl_client *client,
		struct wl_resource *resource, uint32_t id, struct wl_resource *surface_resource) {
	struct flame_eyecandy *eyecandy = wl_resource_get_user_data(resource);
	struct wlr_surface *surface = wlr_surface_from_resource(surface_resource);
	struct surface_glass *glass = glass_get(eyecandy->server, surface);
	if (glass && glass->background) {
		wl_resource_post_error(resource,
			EXT_BACKGROUND_EFFECT_MANAGER_V1_ERROR_BACKGROUND_EFFECT_EXISTS,
			"the surface has a background effect already");
		return;
	}
	struct wl_resource *made = wl_resource_create(client,
		&ext_background_effect_surface_v1_interface, wl_resource_get_version(resource), id);
	if (!made || !glass) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(made, &background_impl, glass, background_resource_destroy);
	glass->background = made;
}

static const struct ext_background_effect_manager_v1_interface background_manager_impl = {
	.destroy = background_manager_handle_destroy,
	.get_background_effect = background_manager_handle_get,
};

static void bind_background(struct wl_client *client, void *data, uint32_t version,
		uint32_t id) {
	struct wl_resource *resource = wl_resource_create(client,
		&ext_background_effect_manager_v1_interface, version, id);
	if (!resource) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(resource, &background_manager_impl, data, NULL);
	ext_background_effect_manager_v1_send_capabilities(resource,
		EXT_BACKGROUND_EFFECT_MANAGER_V1_CAPABILITY_BLUR);
}

// ---- setup ----

bool eyecandy_init(struct flame_eyecandy *eyecandy, struct flame_server *server) {
	eyecandy->server = server;
	// only where flame can draw glass: its own renderer
	if (!server->render.gl.usable) {
		wlr_log(WLR_INFO, "eyecandy: no glass without flame's renderer, not offered");
		return true;
	}
	eyecandy->eyecandy = wl_global_create(server->display, &flame_eyecandy_manager_v1_interface,
		3, eyecandy, bind_eyecandy);
	eyecandy->background = wl_global_create(server->display,
		&ext_background_effect_manager_v1_interface, 1, eyecandy, bind_background);
	return eyecandy->eyecandy && eyecandy->background;
}

void eyecandy_finish(struct flame_eyecandy *eyecandy) {
	if (eyecandy->eyecandy) {
		wl_global_destroy(eyecandy->eyecandy);
		eyecandy->eyecandy = NULL;
	}
	if (eyecandy->background) {
		wl_global_destroy(eyecandy->background);
		eyecandy->background = NULL;
	}
}
