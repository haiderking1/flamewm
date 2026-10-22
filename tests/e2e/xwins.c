// lists X's viewable top-levels the way apps see them: "NAME X,Y WxH",
// bottom to top. all: the mapped ones that aren't viewable too, with
// " unviewable" after. under: the top-level under the pointer, as
// QueryPointer tells it (what drag and drop sources look at), or nothing
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <xcb/xcb.h>

int main(int argc, char **argv) {
	bool all = argc > 1 && !strcmp(argv[1], "all");
	xcb_connection_t *c = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(c)) {
		return 2;
	}
	xcb_screen_t *s = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
	if (argc > 1 && !strcmp(argv[1], "under")) {
		xcb_query_pointer_reply_t *p = xcb_query_pointer_reply(c,
			xcb_query_pointer(c, s->root), NULL);
		xcb_get_property_reply_t *n = p && p->child ? xcb_get_property_reply(c,
			xcb_get_property(c, 0, p->child, XCB_ATOM_WM_NAME, XCB_GET_PROPERTY_TYPE_ANY, 0,
			64), NULL) : NULL;
		printf("under: %.*s\n", n ? xcb_get_property_value_length(n) : 7,
			n ? (char *)xcb_get_property_value(n) : "nothing");
		free(n);
		free(p);
		xcb_disconnect(c);
		return 0;
	}
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
		bool viewable = a && a->map_state == XCB_MAP_STATE_VIEWABLE;
		if (a && g && (viewable || (all && a->map_state == XCB_MAP_STATE_UNVIEWABLE))) {
			printf("%.*s %d,%d %dx%d%s\n", n ? xcb_get_property_value_length(n) : 0,
				n ? (char *)xcb_get_property_value(n) : "", g->x, g->y, g->width, g->height,
				viewable ? "" : " unviewable");
		}
		free(a);
		free(g);
		free(n);
	}
	free(t);
	xcb_disconnect(c);
	return 0;
}
