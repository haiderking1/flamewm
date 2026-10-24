// a window asking for XI2 Enter/Leave and the core ones too, the way
// chrome does (steam's hover menus): the pointer goes in and out again,
// and it says which it heard. chrome only hears the pointer leave from the
// core LeaveNotify
// usage: xcross FLAMECTL
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <xcb/xcb.h>
#include <xcb/xinput.h>

static xcb_connection_t *c;
static const char *ctl;
static uint8_t xi_opcode;
static xcb_window_t w;

static void pointer(int x, int y) {
	char cmd[300];
	snprintf(cmd, sizeof(cmd), "%s pointer move %d %d > /dev/null", ctl, x, y);
	if (system(cmd) != 0) {
		exit(1);
	}
	free(xcb_get_input_focus_reply(c, xcb_get_input_focus(c), NULL));
	nanosleep(&(struct timespec){0, 300000000}, NULL);
}

static void heard(const char *when) {
	int core_enter = 0, core_leave = 0, xi_enter = 0, xi_leave = 0;
	xcb_generic_event_t *e;
	while ((e = xcb_poll_for_event(c))) {
		uint8_t type = e->response_type & 0x7f;
		xcb_ge_generic_event_t *g = (xcb_ge_generic_event_t *)e;
		if (type == XCB_ENTER_NOTIFY && ((xcb_enter_notify_event_t *)e)->event == w) {
			core_enter++;
		} else if (type == XCB_LEAVE_NOTIFY && ((xcb_leave_notify_event_t *)e)->event == w) {
			core_leave++;
		} else if (type == XCB_GE_GENERIC && g->extension == xi_opcode) {
			xi_enter += g->event_type == XCB_INPUT_ENTER;
			xi_leave += g->event_type == XCB_INPUT_LEAVE;
		}
		free(e);
	}
	printf("%s: core enter %d leave %d, xi2 enter %d leave %d\n", when, core_enter, core_leave,
		xi_enter, xi_leave);
}

int main(int argc, char **argv) {
	if (argc < 2) {
		return 2;
	}
	ctl = argv[1];
	c = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(c)) {
		return 2;
	}
	xcb_screen_t *s = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
	const xcb_query_extension_reply_t *xi = xcb_get_extension_data(c, &xcb_input_id);
	xi_opcode = xi ? xi->major_opcode : 0;
	free(xcb_input_xi_query_version_reply(c, xcb_input_xi_query_version(c, 2, 2), NULL));
	// a floating one (a menu): where X has it, the rest of the screen's no X window
	w = xcb_generate_id(c);
	uint32_t values[] = {s->white_pixel, 1,
		XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW};
	xcb_create_window(c, XCB_COPY_FROM_PARENT, w, s->root, 100, 100, 200, 150, 0,
		XCB_WINDOW_CLASS_INPUT_OUTPUT, s->root_visual,
		XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, values);
	struct {
		xcb_input_event_mask_t head;
		uint32_t mask;
	} m = {{XCB_INPUT_DEVICE_ALL_MASTER, 1},
		XCB_INPUT_XI_EVENT_MASK_ENTER | XCB_INPUT_XI_EVENT_MASK_LEAVE};
	xcb_input_xi_select_events(c, w, 1, &m.head);
	xcb_map_window(c, w);
	xcb_flush(c);
	pointer(500, 500);
	heard("before");
	pointer(150, 150);
	heard("in");
	pointer(500, 500);
	heard("out");
	return 0;
}
