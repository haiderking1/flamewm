#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/util/log.h>

#include "xserver/internal.h"

struct xs_window *xs_window_create(struct flame_xserver *xserver, struct xs_client *owner,
		uint32_t id, struct xs_window *parent) {
	struct xs_window *window = calloc(1, sizeof(*window));
	if (!window) {
		return NULL;
	}
	window->id = id;
	window->owner = owner;
	window->parent = parent;
	window->depth = 24;
	window->visual = XS_VISUAL_24;
	window->class = 1;
	window->colormap = XS_COLORMAP;
	wl_list_init(&window->children);
	wl_list_init(&window->properties);
	wl_list_init(&window->interests);
	pixman_region32_init(&window->damage);
	if (parent) {
		wl_list_insert(parent->children.prev, &window->link); // on top
	} else {
		wl_list_init(&window->link);
	}
	wl_list_insert(&xserver->windows, &window->all_link);
	return window;
}

// ---- events about the tree ----

// to the window itself for StructureNotify, its parent for SubstructureNotify.
// the first field says which of the two is hearing it
static void deliver_structure(struct flame_xserver *xserver, struct xs_window *window,
		struct xs_event *event) {
	xs_event_put32(event, 4, window->id);
	xs_event_deliver(xserver, window, XS_MASK_STRUCTURE, event);
	if (window->parent) {
		xs_event_put32(event, 4, window->parent->id);
		xs_event_deliver(xserver, window->parent, XS_MASK_SUBSTRUCTURE, event);
	}
}

static bool inside(const struct xs_window *window, const struct xs_window *ancestor) {
	for (; window; window = window->parent) {
		if (window == ancestor) {
			return true;
		}
	}
	return false;
}

static bool focus_inside(struct flame_xserver *xserver, struct xs_window *window) {
	if (xserver->focus <= XS_FOCUS_POINTER_ROOT) {
		return false;
	}
	struct xs_window *focus = xserver_window(xserver, xserver->focus);
	return focus && inside(focus, window);
}

// ---- mapping ----

void xs_window_map(struct flame_xserver *xserver, struct xs_window *window) {
	if (window->mapped || !window->parent) {
		return;
	}
	// flame is the window manager, nobody redirects: mapping just happens
	window->mapped = true;
	struct xs_event event = {0};
	xs_event_put8(&event, 0, XS_EVENT_MAP_NOTIFY);
	xs_event_put32(&event, 8, window->id);
	xs_event_put8(&event, 12, window->override_redirect);
	deliver_structure(xserver, window, &event);
	// it and whatever's mapped inside get their backgrounds and Expose
	xs_draw_window_shown(xserver, window);
	xs_changed(xserver);
}

void xs_window_unmap(struct flame_xserver *xserver, struct xs_window *window) {
	if (!window->mapped || !window->parent) {
		return;
	}
	window->mapped = false;
	struct xs_event event = {0};
	xs_event_put8(&event, 0, XS_EVENT_UNMAP_NOTIFY);
	xs_event_put32(&event, 8, window->id);
	deliver_structure(xserver, window, &event);
	xs_draw_window_hidden(xserver, window);
	if (focus_inside(xserver, window)) {
		xs_focus_lost(xserver, window);
	}
	xs_changed(xserver);
}

// ---- destroying ----

static void free_window(struct flame_xserver *xserver, struct xs_window *window) {
	struct xs_property *property, *ptmp;
	wl_list_for_each_safe(property, ptmp, &window->properties, link) {
		wl_list_remove(&property->link);
		free(property->data);
		free(property);
	}
	struct xs_interest *interest, *itmp;
	wl_list_for_each_safe(interest, itmp, &window->interests, link) {
		wl_list_remove(&interest->link);
		free(interest);
	}
	struct xs_selection *selection;
	wl_list_for_each(selection, &xserver->selections, link) {
		if (selection->owner == window->id) {
			selection->owner = 0;
			selection->client = NULL;
		}
	}
	xs_present_window_gone(xserver, window->id);
	xs_randr_window_gone(xserver, window->id);
	xs_xi_window_gone(xserver, window->id);
	xs_draw_window_freed(window);
	wl_list_remove(&window->link);
	wl_list_remove(&window->all_link);
	free(window);
}

// children first, the way X reports it
static void destroy_tree(struct flame_xserver *xserver, struct xs_window *window) {
	struct xs_window *child, *tmp;
	wl_list_for_each_reverse_safe(child, tmp, &window->children, link) {
		destroy_tree(xserver, child);
	}
	if (xserver->hooks.window_destroyed && window->parent) {
		xserver->hooks.window_destroyed(xserver->hooks.data, window);
	}
	if (window->parent) {
		wlr_log(WLR_DEBUG, "xserver: window 0x%x (parent 0x%x) destroyed", window->id,
			window->parent->id);
		struct xs_event event = {0};
		xs_event_put8(&event, 0, XS_EVENT_DESTROY_NOTIFY);
		xs_event_put32(&event, 8, window->id);
		deliver_structure(xserver, window, &event);
	}
	free_window(xserver, window);
}

void xs_window_destroy(struct flame_xserver *xserver, struct xs_window *window) {
	if (window->parent) {
		xs_window_unmap(xserver, window);
		if (focus_inside(xserver, window)) {
			xs_focus_lost(xserver, window);
		}
	}
	destroy_tree(xserver, window);
	xs_changed(xserver);
}

// ---- geometry and stacking ----

static void restack(struct xs_window *window, struct xs_window *sibling, uint8_t mode) {
	struct wl_list *children = &window->parent->children;
	wl_list_remove(&window->link);
	// 0 Above, 1 Below. TopIf, BottomIf and Opposite need occlusion nobody
	// here has, they count as Above
	bool below = mode == 1;
	if (sibling && sibling->parent == window->parent) {
		wl_list_insert(below ? sibling->link.prev : &sibling->link, &window->link);
	} else {
		wl_list_insert(below ? children : children->prev, &window->link);
	}
}

void xs_window_configure(struct flame_xserver *xserver, struct xs_window *window,
		uint16_t mask, int x, int y, int width, int height, int border, uint32_t sibling,
		uint8_t stack) {
	int old_x = window->x, old_y = window->y, old_w = window->width, old_h = window->height;
	if (mask & 1) {
		window->x = (int16_t)x;
	}
	if (mask & 2) {
		window->y = (int16_t)y;
	}
	if ((mask & 4) && width > 0) {
		window->width = (uint16_t)width;
	}
	if ((mask & 8) && height > 0) {
		window->height = (uint16_t)height;
	}
	if (mask & 16) {
		window->border = (uint16_t)border;
	}
	bool resized = ((mask & 4) && width > 0) || ((mask & 8) && height > 0);
	if ((mask & 64) && window->parent) {
		restack(window, (mask & 32) ? xserver_window(xserver, sibling) : NULL, stack);
	}

	struct xs_event event = {0};
	xs_event_put8(&event, 0, XS_EVENT_CONFIGURE_NOTIFY);
	xs_event_put32(&event, 8, window->id);
	uint32_t above = 0;
	if (window->parent && window->link.prev != &window->parent->children) {
		struct xs_window *under = wl_container_of(window->link.prev, under, link);
		above = under->id;
	}
	xs_event_put32(&event, 12, above);
	xs_event_put16(&event, 16, (uint16_t)window->x);
	xs_event_put16(&event, 18, (uint16_t)window->y);
	xs_event_put16(&event, 20, window->width);
	xs_event_put16(&event, 22, window->height);
	xs_event_put16(&event, 24, window->border);
	xs_event_put8(&event, 26, window->override_redirect);
	deliver_structure(xserver, window, &event);
	if (window->mapped && (window->x != old_x || window->y != old_y
			|| window->width != old_w || window->height != old_h)) {
		xs_draw_window_moved(xserver, window, old_x, old_y, old_w, old_h);
	}
	if (resized) {
		// vulkan swapchains learn they're the wrong size from this
		xs_present_configure(xserver, window);
	}
	xs_changed(xserver);
}

void xs_window_reparent(struct flame_xserver *xserver, struct xs_window *window,
		struct xs_window *parent, int x, int y) {
	bool was_mapped = window->mapped;
	xs_window_unmap(xserver, window);
	struct xs_window *old = window->parent;
	wl_list_remove(&window->link);
	window->parent = parent;
	wl_list_insert(parent->children.prev, &window->link);
	window->x = (int16_t)x;
	window->y = (int16_t)y;

	struct xs_event event = {0};
	xs_event_put8(&event, 0, XS_EVENT_REPARENT_NOTIFY);
	xs_event_put32(&event, 8, window->id);
	xs_event_put32(&event, 12, parent->id);
	xs_event_put16(&event, 16, (uint16_t)window->x);
	xs_event_put16(&event, 18, (uint16_t)window->y);
	xs_event_put8(&event, 20, window->override_redirect);
	xs_event_put32(&event, 4, window->id);
	xs_event_deliver(xserver, window, XS_MASK_STRUCTURE, &event);
	xs_event_put32(&event, 4, old->id);
	xs_event_deliver(xserver, old, XS_MASK_SUBSTRUCTURE, &event);
	xs_event_put32(&event, 4, parent->id);
	xs_event_deliver(xserver, parent, XS_MASK_SUBSTRUCTURE, &event);

	if (was_mapped) {
		xs_window_map(xserver, window);
	}
	xs_changed(xserver);
}

void xs_window_origin(const struct xs_window *window, int *x, int *y) {
	*x = 0;
	*y = 0;
	for (; window && window->parent; window = window->parent) {
		*x += window->x + window->border;
		*y += window->y + window->border;
	}
}

struct xs_window *xs_window_child_at(struct xs_window *window, int x, int y) {
	struct xs_window *child;
	wl_list_for_each_reverse(child, &window->children, link) {
		if (!child->mapped) {
			continue;
		}
		int cx, cy;
		xs_window_origin(child, &cx, &cy);
		int b = child->border;
		if (x >= cx - b && y >= cy - b && x < cx + child->width + b
				&& y < cy + child->height + b) {
			return child;
		}
	}
	return NULL;
}

// ---- who wants to hear what ----

static struct xs_interest *find_interest(const struct xs_window *window,
		const struct xs_client *client) {
	struct xs_interest *interest;
	wl_list_for_each(interest, &window->interests, link) {
		if (interest->client == client) {
			return interest;
		}
	}
	return NULL;
}

uint32_t xs_window_mask(const struct xs_window *window, const struct xs_client *client) {
	struct xs_interest *interest = find_interest(window, client);
	return interest ? interest->mask : 0;
}

void xs_window_set_mask(struct xs_window *window, struct xs_client *client, uint32_t mask) {
	struct xs_interest *interest = find_interest(window, client);
	if (!mask) {
		if (interest) {
			wl_list_remove(&interest->link);
			free(interest);
		}
		return;
	}
	if (!interest) {
		interest = calloc(1, sizeof(*interest));
		if (!interest) {
			return;
		}
		interest->client = client;
		wl_list_insert(window->interests.prev, &interest->link);
	}
	interest->mask = mask;
}

uint32_t xs_window_all_masks(const struct xs_window *window) {
	uint32_t mask = 0;
	struct xs_interest *interest;
	wl_list_for_each(interest, &window->interests, link) {
		mask |= interest->mask;
	}
	return mask;
}

// ---- properties ----

struct xs_property *xs_window_property(struct xs_window *window, uint32_t name) {
	struct xs_property *property;
	wl_list_for_each(property, &window->properties, link) {
		if (property->name == name) {
			return property;
		}
	}
	return NULL;
}

static void property_notify(struct flame_xserver *xserver, struct xs_window *window,
		uint32_t name, bool deleted) {
	struct xs_event event = {0};
	xs_event_put8(&event, 0, XS_EVENT_PROPERTY_NOTIFY);
	xs_event_put32(&event, 4, window->id);
	xs_event_put32(&event, 8, name);
	xs_event_put32(&event, 12, xs_now(xserver));
	xs_event_put8(&event, 16, deleted);
	xs_event_deliver(xserver, window, XS_MASK_PROPERTY, &event);
	// titles and hints, flame may want to know
	xs_changed(xserver);
}

bool xs_window_change_property(struct flame_xserver *xserver, struct xs_window *window,
		uint8_t mode, uint32_t name, uint32_t type, uint8_t format, uint32_t units,
		const uint8_t *data, uint8_t *error) {
	struct xs_property *property = xs_window_property(window, name);
	size_t unit = format / 8;
	if (property && mode != 0 && (property->format != format || property->type != type)) {
		*error = XS_ERROR_MATCH;
		return false;
	}
	if (!property) {
		property = calloc(1, sizeof(*property));
		if (!property) {
			*error = XS_ERROR_ALLOC;
			return false;
		}
		property->name = name;
		wl_list_insert(window->properties.prev, &property->link);
		mode = 0;
	}
	size_t old = (size_t)property->units * unit, add = (size_t)units * unit;
	size_t total = mode == 0 ? add : old + add;
	uint8_t *buf = malloc(total ? total : 1);
	if (!buf) {
		*error = XS_ERROR_ALLOC;
		return false;
	}
	if (mode == 0) {
		memcpy(buf, data, add);
	} else if (mode == 1) { // prepend
		memcpy(buf, data, add);
		memcpy(buf + add, property->data, old);
	} else { // append
		memcpy(buf, property->data, old);
		memcpy(buf + old, data, add);
	}
	free(property->data);
	property->data = buf;
	property->units = (uint32_t)(total / unit);
	property->type = type;
	property->format = format;
	property_notify(xserver, window, name, false);
	return true;
}

void xs_window_delete_property(struct flame_xserver *xserver, struct xs_window *window,
		uint32_t name) {
	struct xs_property *property = xs_window_property(window, name);
	if (!property) {
		return;
	}
	wl_list_remove(&property->link);
	free(property->data);
	free(property);
	property_notify(xserver, window, name, true);
}

// ---- input focus ----

static void focus_event(struct flame_xserver *xserver, uint8_t type, uint32_t id) {
	struct xs_window *window = id > XS_FOCUS_POINTER_ROOT ? xserver_window(xserver, id) : NULL;
	if (!window) {
		return;
	}
	struct xs_event event = {0};
	xs_event_put8(&event, 0, type);
	xs_event_put8(&event, 1, 3); // NotifyNonlinear
	xs_event_put32(&event, 4, window->id);
	xs_event_put8(&event, 8, 0); // NotifyNormal
	int evtype = type == XS_EVENT_FOCUS_IN ? XS_XI_FOCUS_IN : XS_XI_FOCUS_OUT;
	xs_event_deliver_input(xserver, window, XS_MASK_FOCUS, evtype, &event);
	xs_xi_focus(xserver, evtype, window);
}

void xs_focus_set(struct flame_xserver *xserver, uint32_t focus, uint8_t revert) {
	uint32_t old = xserver->focus;
	xserver->focus_revert = revert;
	if (focus == old) {
		return;
	}
	xserver->focus = focus;
	focus_event(xserver, XS_EVENT_FOCUS_OUT, old);
	focus_event(xserver, XS_EVENT_FOCUS_IN, focus);
}

void xs_focus_lost(struct flame_xserver *xserver, struct xs_window *window) {
	uint32_t focus = XS_FOCUS_NONE;
	if (xserver->focus_revert == 1) {
		focus = XS_FOCUS_POINTER_ROOT;
	} else if (xserver->focus_revert == 2) {
		// the closest ancestor still on screen, or nothing
		for (struct xs_window *up = window->parent; up; up = up->parent) {
			if (up->parent && xs_window_viewable(up)) {
				focus = up->id;
				break;
			}
		}
	}
	xs_focus_set(xserver, focus, 0);
	if (xserver->hooks.focus_changed) {
		xserver->hooks.focus_changed(xserver->hooks.data, focus);
	}
}

void xs_pixmap_destroy(struct flame_xserver *xserver, struct xs_pixmap *pixmap) {
	if (xserver->hooks.pixmap_destroyed) {
		xserver->hooks.pixmap_destroyed(xserver->hooks.data, pixmap);
	}
	for (int i = 0; i < pixmap->planes; i++) {
		close(pixmap->fds[i]);
	}
	if (pixmap->image) {
		pixman_image_unref(pixmap->image);
	}
	if (pixmap->shm) {
		xs_shm_unref(pixmap->shm);
	}
	wl_list_remove(&pixmap->link);
	free(pixmap);
}

// ---- a client leaving ----

void xs_client_resources_free(struct flame_xserver *xserver, struct xs_client *client) {
	// its windows, each from the highest one it owns down
	bool again = true;
	while (again) {
		again = false;
		struct xs_window *window;
		wl_list_for_each(window, &xserver->windows, all_link) {
			if (window->owner == client) {
				xs_window_destroy(xserver, window);
				again = true;
				break;
			}
		}
	}
	// what it asked to hear about on everyone else's
	struct xs_window *window;
	wl_list_for_each(window, &xserver->windows, all_link) {
		xs_window_set_mask(window, client, 0);
	}
	struct xs_pixmap *pixmap, *ptmp;
	wl_list_for_each_safe(pixmap, ptmp, &xserver->pixmaps, link) {
		if (pixmap->owner == client) {
			xs_pixmap_destroy(xserver, pixmap);
		}
	}
	struct xs_gc *gc, *gtmp;
	wl_list_for_each_safe(gc, gtmp, &xserver->gcs, link) {
		if (gc->owner == client) {
			xs_gc_destroy(gc);
		}
	}
	xs_shm_detach_all(xserver, client);
	xs_render_client_gone(xserver, client);
	xs_randr_client_gone(xserver, client);
	xs_xi_client_gone(xserver, client);
	struct xs_fence *fence, *ftmp;
	wl_list_for_each_safe(fence, ftmp, &xserver->fences, link) {
		if (fence->owner == client) {
			xs_fence_destroy(fence);
		}
	}
	struct xs_syncobj *syncobj, *stmp;
	wl_list_for_each_safe(syncobj, stmp, &xserver->syncobjs, link) {
		if (syncobj->owner == client) {
			xs_syncobj_destroy(xserver, syncobj);
		}
	}
	struct xs_region *region, *rtmp;
	wl_list_for_each_safe(region, rtmp, &xserver->regions, link) {
		if (region->owner == client) {
			wl_list_remove(&region->link);
			free(region);
		}
	}
	struct xs_present_select *select, *sstmp;
	wl_list_for_each_safe(select, sstmp, &xserver->present_selects, link) {
		if (select->client == client) {
			wl_list_remove(&select->link);
			free(select);
		}
	}
	struct xs_selection *selection;
	wl_list_for_each(selection, &xserver->selections, link) {
		if (selection->client == client) {
			selection->owner = 0;
			selection->client = NULL;
		}
	}
}
