#include "picker/picker.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

#include "capture/capture.h"
#include "core/server.h"
#include "desktop/view.h"
#include "ipc/ipc.h"
#include "render/text.h"
#include "scene/scene.h"
#include "util/listener.h"

// the overlay, top down: the bar (glass, its knob, the labels), the panel on
// what's lit (glass, two lines), a light ring around what's lit, the dim
// around it. what's lit and the knob glide to wherever they go next

#define DIM 0.42f
#define RING 2
#define BAR_TOP 18
#define BAR_PAD 4 // around the choices, inside the bar
#define CHOICE_H 30
#define CHOICE_PAD 16 // left and right of a label
#define PANEL_PAD_X 24
#define PANEL_PAD_Y 14
#define PANEL_GAP 3
#define PANEL_RADIUS 20
#define REFRESH_MS 100
#define CHOICE_FONT "sans medium 10.5"
#define TITLE_FONT "sans 10"
#define ACTION_FONT "sans bold 12.5"

static const struct flame_curve ease_out = {0.2, 0.8, 0.2, 1.0};
static const struct flame_curve ease_in = {0.4, 0.0, 1.0, 1.0};

static const char *const choice_text[PICKER_CHOICES] = {"Window", "Entire Screen", "Cancel"};

// glass that reads over anything: darkened a little so white text stays
// legible on a bright window, its colors pushed, a lit rim
static const struct flame_glass_style bar_style = {
	.blur = 20,
	.tint = {0, 0, 0, 0.28f},
	.saturation = 1.8f,
	.edge = 0.55f,
	.refraction = 5,
};

static const struct flame_glass_style knob_style = {
	.blur = 4,
	.tint = {0.17f, 0.17f, 0.17f, 0.17f},
	.saturation = 1.2f,
	.edge = 0.7f,
	.refraction = 2,
};

static const struct flame_glass_style panel_style = {
	.radius = PANEL_RADIUS,
	.blur = 22,
	.tint = {0, 0, 0, 0.32f},
	.saturation = 1.8f,
	.edge = 0.6f,
	.refraction = 7,
};

// ---- labels ----

static void label_set(struct flame_picker *picker, struct picker_label *label,
		struct flame_tree *parent, const char *text, const char *font, const float color[4],
		int max_width) {
	if (!label->node) {
		label->node = scene_buffer_create(parent, NULL, NULL);
	}
	struct wlr_texture *texture = render_text(picker->server->render.renderer, text, font,
		color, max_width, picker->scale, &label->width, &label->height);
	if (label->node) {
		scene_buffer_set(label->node, texture, texture, texture ? label->width : 0,
			texture ? label->height : 0, false, NULL, 0);
	}
	if (label->texture) {
		wlr_texture_destroy(label->texture);
	}
	label->texture = texture;
	if (!texture) {
		label->width = label->height = 0;
	}
}

static void label_free(struct picker_label *label) {
	if (label->texture) {
		wlr_texture_destroy(label->texture);
	}
	*label = (struct picker_label){0};
}

// ---- what's under the pointer, what it answers ----

// the window under a point, on the workspace showing: the scene without us
static struct flame_view *view_at(struct flame_picker *picker, double lx, double ly) {
	struct flame_desktop *desktop = &picker->server->desktop;
	bool enabled = picker->tree->node.enabled;
	picker->tree->node.enabled = false;
	struct wlr_surface *surface;
	double sx, sy;
	struct flame_view *view = desktop_view_at(desktop, lx, ly, &surface, &sx, &sy);
	picker->tree->node.enabled = enabled;
	return view && view->workspace == desktop->active && view->shown ? view : NULL;
}

// "Window: ID", NULL when the window isn't on the portal's list yet
static char *answer_for(struct flame_picker *picker) {
	char buf[256];
	if (picker->mode == PICKER_SCREEN) {
		struct flame_output *output;
		wl_list_for_each(output, &picker->server->outputs.list, link) {
			if (wlr_box_equal(&output->box, &picker->area)) {
				snprintf(buf, sizeof(buf), "Monitor: %s", output->wlr->name);
				return strdup(buf);
			}
		}
		return NULL;
	}
	const char *id = picker->view
		? capture_identifier(&picker->server->capture, picker->view) : NULL;
	if (!id) {
		return NULL;
	}
	snprintf(buf, sizeof(buf), "Window: %s", id);
	return strdup(buf);
}

// ---- how it looks right now ----

static struct wlr_box box_lerp(struct wlr_box a, struct wlr_box b, double t) {
	if (wlr_box_empty(&a)) {
		return b; // nothing lit before: it shows up where it goes
	}
	if (wlr_box_empty(&b)) {
		return t < 1 ? a : b;
	}
	return (struct wlr_box){
		(int)lround(lerp(a.x, b.x, t)), (int)lround(lerp(a.y, b.y, t)),
		(int)lround(lerp(a.width, b.width, t)), (int)lround(lerp(a.height, b.height, t)),
	};
}

static void set_rect(struct flame_rect *rect, int x, int y, int width, int height,
		const float color[4]) {
	scene_node_set_position(&rect->node, x, y);
	scene_rect_set_size(rect, width > 0 ? width : 0, height > 0 ? height : 0);
	scene_rect_set_color(rect, color);
}

// the dim around what's lit, the ring around it, the panel on it
static void apply(struct flame_picker *picker) {
	float shown = (float)picker->shown;
	struct wlr_box a = picker->area, l = picker->lit;
	const float dim[4] = {0, 0, 0, DIM * shown};
	if (wlr_box_empty(&l)) {
		set_rect(picker->dim[0], a.x, a.y, a.width, a.height, dim);
		for (int i = 1; i < 4; i++) {
			set_rect(picker->dim[i], 0, 0, 0, 0, dim);
		}
	} else {
		set_rect(picker->dim[0], a.x, a.y, a.width, l.y - a.y, dim);
		set_rect(picker->dim[1], a.x, l.y + l.height, a.width,
			a.y + a.height - l.y - l.height, dim);
		set_rect(picker->dim[2], a.x, l.y, l.x - a.x, l.height, dim);
		set_rect(picker->dim[3], l.x + l.width, l.y, a.x + a.width - l.x - l.width, l.height,
			dim);
	}
	// a ring only around a window, the whole screen has its edges
	bool ring = !wlr_box_empty(&l) && picker->mode == PICKER_WINDOW;
	float r = ring ? 0.55f * shown : 0;
	const float light[4] = {r, r, r, r};
	set_rect(picker->ring[0], l.x, l.y, l.width, RING, light);
	set_rect(picker->ring[1], l.x, l.y + l.height - RING, l.width, RING, light);
	set_rect(picker->ring[2], l.x, l.y + RING, RING, l.height - 2 * RING, light);
	set_rect(picker->ring[3], l.x + l.width - RING, l.y + RING, RING, l.height - 2 * RING,
		light);

	// the panel in the middle of what's lit, kept on the output
	int pw = (picker->title.width > picker->action.width ? picker->title.width
		: picker->action.width) + 2 * PANEL_PAD_X;
	int ph = picker->title.height + PANEL_GAP + picker->action.height + 2 * PANEL_PAD_Y;
	int px = l.x + (l.width - pw) / 2, py = l.y + (l.height - ph) / 2;
	px = px < a.x + 8 ? a.x + 8 : px;
	py = py < a.y + 8 ? a.y + 8 : py;
	scene_node_set_enabled(&picker->panel->node, !wlr_box_empty(&l));
	scene_node_set_position(&picker->panel->node, px, py);
	scene_glass_set_size(picker->panel_glass, pw, ph);
	if (picker->title.node) {
		scene_node_set_position(&picker->title.node->node, (pw - picker->title.width) / 2,
			PANEL_PAD_Y);
	}
	if (picker->action.node) {
		scene_node_set_position(&picker->action.node->node, (pw - picker->action.width) / 2,
			PANEL_PAD_Y + picker->title.height + PANEL_GAP);
	}
	struct flame_effect faded = {.opacity = shown, .scale_x = 1, .scale_y = 1};
	scene_tree_set_effect(picker->panel, &faded);

	// the bar drops in from above as it fades in
	struct wlr_box k = picker->knob_box;
	scene_node_set_position(&picker->knob->node, k.x - picker->bar_box.x,
		k.y - picker->bar_box.y);
	scene_glass_set_size(picker->knob, k.width, k.height);
	struct flame_effect dropped = faded;
	dropped.dy = -12 * (1 - picker->shown);
	scene_tree_set_effect(picker->bar, &dropped);
}

static void glide_step(struct flame_anim *anim, double eased, double t) {
	struct flame_picker *picker = anim->data;
	picker->lit = box_lerp(picker->lit_from, picker->lit_to, eased);
	picker->knob_box = box_lerp(picker->knob_from, picker->knob_to, eased);
	apply(picker);
}

// what's lit and the knob head for where they go now: the window under
// the pointer, the whole screen, nothing
static void retarget(struct flame_picker *picker, bool animate) {
	struct wlr_box lit = {0};
	if (picker->mode == PICKER_SCREEN) {
		lit = picker->area;
	} else if (picker->view) {
		wlr_box_intersection(&lit, &picker->view->geometry, &picker->area);
	}
	struct wlr_box knob = picker->choice_boxes[picker->mode];
	if (wlr_box_equal(&lit, &picker->lit_to) && wlr_box_equal(&knob, &picker->knob_to)) {
		return;
	}
	picker->lit_from = picker->lit;
	picker->lit_to = lit;
	picker->knob_from = picker->knob_box;
	picker->knob_to = knob;
	anim_start(&picker->server->anims, &picker->glide, animate ? 180 : 0, &ease_out);
}

// the panel's two lines for what's lit now
static void relabel(struct flame_picker *picker) {
	const char *title = NULL, *action = NULL;
	char name[128] = "";
	if (picker->mode == PICKER_SCREEN) {
		struct flame_output *output;
		wl_list_for_each(output, &picker->server->outputs.list, link) {
			if (wlr_box_equal(&output->box, &picker->area)) {
				snprintf(name, sizeof(name), "%s", output->wlr->name);
			}
		}
		title = name;
		action = "Share Entire Screen";
	} else if (picker->view) {
		const char *t = view_title(picker->view), *app = view_app_id(picker->view);
		title = t && *t ? t : app && *app ? app : "window";
		action = "Share This Window";
	}
	if (!title) {
		return; // nothing lit, the panel's off anyway
	}
	int max_width = picker->area.width / 2;
	label_set(picker, &picker->title, picker->panel, title, TITLE_FONT,
		(const float[4]){1, 1, 1, 0.72f}, max_width);
	label_set(picker, &picker->action, picker->panel, action, ACTION_FONT,
		(const float[4]){1, 1, 1, 1}, max_width);
}

// view lit from now on (NULL for none)
static void light(struct flame_picker *picker, struct flame_view *view) {
	if (view == picker->view) {
		return;
	}
	picker->view = view;
	relabel(picker);
	retarget(picker, true);
}

static void set_mode(struct flame_picker *picker, enum picker_mode mode) {
	if (mode == picker->mode || !(picker->kinds & (1u << mode))) {
		return; // there already, or the app didn't ask for it
	}
	picker->mode = mode;
	relabel(picker);
	retarget(picker, true);
}

// ---- building it ----

static struct flame_output *output_under_pointer(struct flame_server *server) {
	struct wlr_cursor *cursor = server->input.cursor.wlr;
	struct wlr_output *wlr = wlr_output_layout_output_at(server->outputs.layout, cursor->x,
		cursor->y);
	struct flame_output *output;
	wl_list_for_each(output, &server->outputs.list, link) {
		if (output->wlr == wlr) {
			return output;
		}
	}
	return outputs_primary(&server->outputs);
}

// the bar's labels, their boxes, the bar around them, centered at the top
static void build_bar(struct flame_picker *picker) {
	int x = 0;
	for (int i = 0; i < PICKER_CHOICES; i++) {
		struct picker_label *label = &picker->choices[i];
		// cancel quieter, what the app didn't ask for faded out
		bool offered = i == PICKER_CHOICES - 1 || picker->kinds & (1u << i);
		label_set(picker, label, picker->bar, choice_text[i], CHOICE_FONT,
			(const float[4]){1, 1, 1, !offered ? 0.3f : i == PICKER_CHOICES - 1 ? 0.7f : 1},
			400);
		picker->choice_boxes[i] = (struct wlr_box){x, 0, label->width + 2 * CHOICE_PAD,
			CHOICE_H};
		x += picker->choice_boxes[i].width;
	}
	int width = x + 2 * BAR_PAD, height = CHOICE_H + 2 * BAR_PAD;
	picker->bar_box = (struct wlr_box){picker->area.x + (picker->area.width - width) / 2,
		picker->area.y + BAR_TOP, width, height};
	scene_node_set_position(&picker->bar->node, picker->bar_box.x, picker->bar_box.y);
	struct flame_glass_style bar = bar_style;
	bar.radius = height / 2.0;
	scene_glass_set_style(picker->bar_glass, &bar);
	scene_glass_set_size(picker->bar_glass, width, height);
	struct flame_glass_style knob = knob_style;
	knob.radius = CHOICE_H / 2.0;
	scene_glass_set_style(picker->knob, &knob);
	for (int i = 0; i < PICKER_CHOICES; i++) {
		struct wlr_box *box = &picker->choice_boxes[i];
		struct picker_label *label = &picker->choices[i];
		if (label->node) {
			scene_node_set_position(&label->node->node, BAR_PAD + box->x + CHOICE_PAD,
				BAR_PAD + (CHOICE_H - label->height) / 2);
			scene_node_raise_to_top(&label->node->node);
		}
		// layout coords from here on
		box->x += picker->bar_box.x + BAR_PAD;
		box->y = picker->bar_box.y + BAR_PAD;
	}
}

// everything it showed goes, the tree stays for next time
static void clear(struct flame_picker *picker) {
	for (int i = 0; i < PICKER_CHOICES; i++) {
		if (picker->choices[i].node) {
			scene_node_destroy(&picker->choices[i].node->node);
		}
		label_free(&picker->choices[i]);
	}
	if (picker->title.node) {
		scene_node_destroy(&picker->title.node->node);
	}
	if (picker->action.node) {
		scene_node_destroy(&picker->action.node->node);
	}
	label_free(&picker->title);
	label_free(&picker->action);
	picker->view = NULL;
	scene_node_set_enabled(&picker->tree->node, false);
}

// ---- opening, answering, closing ----

static void fade_step(struct flame_anim *anim, double eased, double t) {
	struct flame_picker *picker = anim->data;
	picker->shown = lerp(picker->shown_from, picker->shown_to, eased);
	apply(picker);
}

// from where it is now, open or closed
static void fade_to(struct flame_picker *picker, double to, int ms,
		const struct flame_curve *curve) {
	picker->shown_from = picker->shown;
	picker->shown_to = to;
	anim_start(&picker->server->anims, &picker->fade, ms, curve);
}

static void fade_done(struct flame_anim *anim, bool finished) {
	struct flame_picker *picker = anim->data;
	if (!picker->open && finished) {
		clear(picker);
	}
}

static void stop_waiting(struct flame_picker *picker) {
	if (picker->client) {
		listener_disconnect(&picker->client_destroy);
		picker->client = NULL;
	}
}

// answer NULL: cancelled
static void finish(struct flame_picker *picker, const char *answer) {
	if (!picker->open) {
		return;
	}
	if (picker->client) {
		if (answer) {
			ipc_client_send(picker->client, answer, strlen(answer));
		} else {
			static const char cancelled[] = "{\"ok\":false,\"error\":\"cancelled\"}";
			ipc_client_send(picker->client, cancelled, sizeof(cancelled) - 1);
		}
		stop_waiting(picker);
	}
	wlr_log(WLR_DEBUG, "picker: %s", answer ? answer : "cancelled");
	picker->open = false;
	wl_event_source_timer_update(picker->refresh, 0);
	fade_to(picker, 0, 140, &ease_in);
	// the pointer and the keyboard go back to whatever's under them
	cursor_refresh(&picker->server->input.cursor);
}

// what's lit is shared, if anything is
static void share(struct flame_picker *picker) {
	char *answer = answer_for(picker);
	if (answer) {
		finish(picker, answer);
		free(answer);
	}
}

static void handle_client_destroy(struct wl_listener *listener, void *data) {
	struct flame_picker *picker = wl_container_of(listener, picker, client_destroy);
	stop_waiting(picker); // nobody to answer: closed like a cancel
	finish(picker, NULL);
}

// windows move, resize and close, workspaces switch: what's lit follows,
// the window under the pointer when the lit one's gone from view
static void follow(struct flame_picker *picker) {
	if (!picker->open || picker->mode != PICKER_WINDOW) {
		return;
	}
	struct flame_desktop *desktop = &picker->server->desktop;
	struct flame_view *view = picker->view;
	if (!view || view->workspace != desktop->active || !view->shown) {
		struct wlr_cursor *cursor = picker->server->input.cursor.wlr;
		view = view_at(picker, cursor->x, cursor->y);
		if (!view) {
			view = desktop->focused && desktop->focused->workspace == desktop->active
				? desktop->focused : NULL;
		}
		light(picker, view);
		return;
	}
	retarget(picker, true);
}

static int handle_refresh(void *data) {
	struct flame_picker *picker = data;
	follow(picker);
	if (picker->open) {
		wl_event_source_timer_update(picker->refresh, REFRESH_MS);
	}
	return 0;
}

// the one screen there is, when the app only wants a screen: nothing to
// pick. NULL with more than one
static struct flame_output *only_output(struct flame_server *server) {
	struct flame_output *output, *only = NULL;
	wl_list_for_each(output, &server->outputs.list, link) {
		if (output->wlr->enabled) {
			if (only) {
				return NULL;
			}
			only = output;
		}
	}
	return only;
}

void picker_open(struct flame_picker *picker, struct flame_ipc_client *client,
		unsigned kinds) {
	struct flame_server *server = picker->server;
	kinds &= PICKER_WINDOWS | PICKER_SCREENS;
	kinds = kinds ? kinds : PICKER_WINDOWS | PICKER_SCREENS;
	struct flame_output *only = kinds == PICKER_SCREENS ? only_output(server) : NULL;
	if (only) {
		// the app's own "share your screen" already said which
		char answer[128];
		snprintf(answer, sizeof(answer), "Monitor: %s", only->wlr->name);
		wlr_log(WLR_DEBUG, "picker: asked for a screen, there's one: %s", answer);
		ipc_client_send(client, answer, strlen(answer));
		return;
	}
	if (picker->open) {
		finish(picker, NULL);
	}
	anim_cancel(&picker->fade);
	anim_cancel(&picker->glide);
	clear(picker);
	struct flame_output *output = output_under_pointer(server);
	if (!output) {
		static const char none[] = "{\"ok\":false,\"error\":\"no screen to pick on\"}";
		ipc_client_send(client, none, sizeof(none) - 1);
		return;
	}
	picker->area = output->box;
	picker->scale = output->wlr->scale > 0 ? output->wlr->scale : 1;
	picker->client = client;
	listener_connect(&picker->client_destroy, &client->events.destroy, handle_client_destroy);
	picker->open = true;
	picker->shown = 0;
	picker->kinds = kinds;
	picker->mode = kinds & PICKER_WINDOWS ? PICKER_WINDOW : PICKER_SCREEN;
	picker->lit = picker->lit_from = picker->lit_to = (struct wlr_box){0};
	picker->knob_box = picker->knob_from = picker->knob_to = (struct wlr_box){0};

	scene_node_raise_to_top(&picker->tree->node);
	scene_node_set_enabled(&picker->tree->node, true);
	build_bar(picker);
	struct wlr_cursor *cursor = server->input.cursor.wlr;
	picker->view = NULL;
	follow(picker);
	// no glide on opening, it's all there as it fades in
	retarget(picker, false);
	anim_finish(&picker->glide);
	apply(picker);
	wlr_log(WLR_DEBUG, "picker: open on %s for %s, %s lit", output->wlr->name,
		kinds == PICKER_WINDOWS ? "a window" : kinds == PICKER_SCREENS ? "a screen"
		: "a window or a screen", picker->view ? "a window" : "nothing");

	// the pointer is the picker's: nothing under it hears it go by
	wlr_seat_pointer_clear_focus(server->input.seat);
	picker_pointer_motion(picker, cursor->x, cursor->y);
	wl_event_source_timer_update(picker->refresh, REFRESH_MS);
	fade_to(picker, 1, 220, &ease_out);
}

bool picker_active(const struct flame_picker *picker) {
	return picker->open;
}

// ---- input ----

static int choice_at(struct flame_picker *picker, double lx, double ly) {
	for (int i = 0; i < PICKER_CHOICES; i++) {
		bool offered = i == PICKER_CHOICES - 1 || picker->kinds & (1u << i);
		if (offered && wlr_box_contains_point(&picker->choice_boxes[i], lx, ly)) {
			return i;
		}
	}
	return -1;
}

void picker_pointer_motion(struct flame_picker *picker, double lx, double ly) {
	bool on_bar = wlr_box_contains_point(&picker->bar_box, lx, ly);
	if (!on_bar && picker->mode == PICKER_WINDOW) {
		light(picker, view_at(picker, lx, ly));
	}
	bool clickable = on_bar ? choice_at(picker, lx, ly) >= 0
		: picker->mode == PICKER_SCREEN ? wlr_box_contains_point(&picker->area, lx, ly)
		: picker->view != NULL;
	cursor_set_name(&picker->server->input.cursor, clickable ? "pointer" : "default");
}

void picker_pointer_button(struct flame_picker *picker, double lx, double ly, bool pressed) {
	if (!pressed) {
		return;
	}
	if (wlr_box_contains_point(&picker->bar_box, lx, ly)) {
		int choice = choice_at(picker, lx, ly);
		if (choice == PICKER_CHOICES - 1) {
			finish(picker, NULL);
		} else if (choice >= 0) {
			set_mode(picker, (enum picker_mode)choice);
		}
		return;
	}
	if (picker->mode == PICKER_WINDOW) {
		light(picker, view_at(picker, lx, ly));
	}
	// a click on nothing does nothing: escape or cancel closes it
	share(picker);
}

// the next window on the workspace showing, by tab
static void step(struct flame_picker *picker, bool back) {
	struct flame_workspace *workspace = picker->server->desktop.active;
	struct flame_view *views[256];
	int n = 0;
	struct flame_view *view;
	wl_list_for_each(view, &workspace->views, link) {
		if (view->shown && n < 256) {
			views[n++] = view;
		}
	}
	wl_list_for_each(view, &workspace->floats, link) {
		if (view->shown && n < 256) {
			views[n++] = view;
		}
	}
	if (n == 0) {
		return;
	}
	int at = -1;
	for (int i = 0; i < n; i++) {
		at = views[i] == picker->view ? i : at;
	}
	at = at < 0 ? 0 : ((at + (back ? -1 : 1)) % n + n) % n;
	light(picker, views[at]);
}

static void key_press(struct flame_picker *picker, xkb_keysym_t sym) {
	switch (sym) {
	case XKB_KEY_Escape:
		finish(picker, NULL);
		return;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
	case XKB_KEY_space:
		share(picker);
		return;
	case XKB_KEY_Tab:
	case XKB_KEY_Down:
		set_mode(picker, PICKER_WINDOW);
		if (picker->mode == PICKER_WINDOW) {
			step(picker, false);
		}
		return;
	case XKB_KEY_ISO_Left_Tab:
	case XKB_KEY_Up:
		set_mode(picker, PICKER_WINDOW);
		if (picker->mode == PICKER_WINDOW) {
			step(picker, true);
		}
		return;
	case XKB_KEY_Left:
		set_mode(picker, PICKER_WINDOW);
		return;
	case XKB_KEY_Right:
		set_mode(picker, PICKER_SCREEN);
		return;
	default:
		return;
	}
}

bool picker_key(struct flame_picker *picker, uint32_t keycode, xkb_keysym_t sym,
		bool pressed, uint32_t modifiers) {
	uint8_t byte = (uint8_t)(keycode / 8 % 32), bit = (uint8_t)(1u << (keycode % 8));
	if (!pressed) {
		bool ours = picker->eaten[byte] & bit;
		picker->eaten[byte] &= (uint8_t)~bit;
		return ours;
	}
	if (!picker->open) {
		return false;
	}
	if (modifiers & (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO)) {
		// a bind: super+2 goes to workspace 2, what's lit follows right after
		wl_event_source_timer_update(picker->refresh, 30);
		return false;
	}
	picker->eaten[byte] |= bit;
	key_press(picker, sym);
	return true;
}

void picker_view_gone(struct flame_picker *picker, struct flame_view *view) {
	if (picker->view == view) {
		picker->view = NULL;
		retarget(picker, true);
		wl_event_source_timer_update(picker->refresh, 1);
	}
}

// ---- setup ----

bool picker_init(struct flame_picker *picker, struct flame_server *server) {
	picker->server = server;
	wl_list_init(&picker->client_destroy.link);
	picker->fade = (struct flame_anim){.step = fade_step, .done = fade_done, .data = picker};
	wl_list_init(&picker->fade.link);
	picker->glide = (struct flame_anim){.step = glide_step, .data = picker};
	wl_list_init(&picker->glide.link);
	picker->tree = scene_tree_create(&server->render.scene.root);
	picker->refresh = wl_event_loop_add_timer(server->loop, handle_refresh, picker);
	if (!picker->tree || !picker->refresh) {
		wlr_log(WLR_ERROR, "failed to set up the share picker");
		return false;
	}
	static const float clear_color[4] = {0, 0, 0, 0};
	bool ok = true;
	for (int i = 0; i < 4; i++) {
		picker->dim[i] = scene_rect_create(picker->tree, 0, 0, clear_color);
		ok = ok && picker->dim[i];
	}
	for (int i = 0; i < 4; i++) {
		picker->ring[i] = scene_rect_create(picker->tree, 0, 0, clear_color);
		ok = ok && picker->ring[i];
	}
	picker->panel = scene_tree_create(picker->tree);
	picker->panel_glass = picker->panel
		? scene_glass_create(picker->panel, 0, 0, &panel_style) : NULL;
	picker->bar = scene_tree_create(picker->tree);
	picker->bar_glass = picker->bar ? scene_glass_create(picker->bar, 0, 0, &bar_style) : NULL;
	picker->knob = picker->bar ? scene_glass_create(picker->bar, 0, 0, &knob_style) : NULL;
	if (!ok || !picker->panel_glass || !picker->bar_glass || !picker->knob) {
		wlr_log(WLR_ERROR, "failed to set up the share picker");
		return false;
	}
	scene_node_set_enabled(&picker->tree->node, false);
	return true;
}

void picker_finish(struct flame_picker *picker) {
	stop_waiting(picker);
	picker->open = false;
	anim_cancel(&picker->fade);
	anim_cancel(&picker->glide);
	if (picker->tree) {
		clear(picker);
	}
	if (picker->refresh) {
		wl_event_source_remove(picker->refresh);
		picker->refresh = NULL;
	}
}
