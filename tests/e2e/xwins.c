// lists X's mapped top-levels the way apps see them: "NAME X,Y WxH",
// bottom to top
#include <stdio.h>
#include <stdlib.h>
#include <xcb/xcb.h>

int main(void) {
	xcb_connection_t *c = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(c)) {
		return 2;
	}
	xcb_screen_t *s = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
	xcb_query_tree_reply_t *t = xcb_query_tree_reply(c, xcb_query_tree(c, s->root), NULL);
	if (!t) {
		return 1;
	}
	xcb_window_t *kids = xcb_query_tree_children(t);
	for (int i = 0; i < xcb_query_tree_children_length(t); i++) {
		xcb_get_window_attributes_reply_t *a = xcb_get_window_attributes_reply(c,
			xcb_get_window_attributes(c, kids[i]), NULL);
		xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(c, xcb_get_geometry(c, kids[i]),
			NULL);
		xcb_get_property_reply_t *n = xcb_get_property_reply(c, xcb_get_property(c, 0, kids[i],
			XCB_ATOM_WM_NAME, XCB_GET_PROPERTY_TYPE_ANY, 0, 64), NULL);
		if (a && g && a->map_state == XCB_MAP_STATE_VIEWABLE) {
			printf("%.*s %d,%d %dx%d\n", n ? xcb_get_property_value_length(n) : 0,
				n ? (char *)xcb_get_property_value(n) : "", g->x, g->y, g->width, g->height);
		}
		free(a);
		free(g);
		free(n);
	}
	free(t);
	xcb_disconnect(c);
	return 0;
}
