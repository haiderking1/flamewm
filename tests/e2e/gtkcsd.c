// a gtk3 window with client side decorations: a headerbar, and a shadow
// when it floats. usage: gtkcsd TITLE [dialog] [SECONDS]
#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
	gtk_init(&argc, &argv);
	GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	GtkWidget *bar = gtk_header_bar_new();
	gtk_header_bar_set_title(GTK_HEADER_BAR(bar), argc > 1 ? argv[1] : "csd");
	gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(bar), TRUE);
	gtk_window_set_titlebar(GTK_WINDOW(win), bar);
	gtk_window_set_title(GTK_WINDOW(win), argc > 1 ? argv[1] : "csd");
	if (argc > 2 && !strcmp(argv[2], "dialog")) {
		gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_DIALOG);
	}
	gtk_window_set_default_size(GTK_WINDOW(win), 400, 250);
	gtk_container_add(GTK_CONTAINER(win), gtk_label_new("client side decorations"));
	g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
	gtk_widget_show_all(win);
	g_timeout_add_seconds(argc > 3 ? (guint)atoi(argv[3]) : 10, (GSourceFunc)gtk_main_quit, NULL);
	gtk_main();
	return 0;
}
