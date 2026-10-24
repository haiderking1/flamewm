// an X video player keeping the screen on: MIT-SCREEN-SAVER Suspend for
// SECONDS, then it goes
// usage: xsuspend SECONDS
#include <stdlib.h>
#include <unistd.h>
#include <xcb/screensaver.h>
#include <xcb/xcb.h>

int main(int argc, char **argv) {
	xcb_connection_t *c = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(c)) {
		return 2;
	}
	xcb_screensaver_suspend(c, 1);
	free(xcb_get_input_focus_reply(c, xcb_get_input_focus(c), NULL));
	sleep(argc > 1 ? (unsigned)atoi(argv[1]) : 2);
	xcb_disconnect(c);
	return 0;
}
