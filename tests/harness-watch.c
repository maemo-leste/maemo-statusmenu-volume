/* Watch harness: load the plugin and report the slider position every 500 ms,
 * so a ladder switch caused by an appearing/disappearing call stream is
 * visible without driving the slider.
 *
 *   gcc -O0 -g -o tests/out/harness-watch tests/harness-watch.c \
 *       $(pkg-config --cflags --libs gtk+-2.0 libhildondesktop-1)
 *   G_MESSAGES_DEBUG=all tests/out/harness-watch src/.libs/volume_status_menu_item.so
 */
#include <gtk/gtk.h>
#include <libhildondesktop/libhildondesktop.h>
#include <stdio.h>

static GtkWidget *find_range(GtkWidget *w) {
	if (GTK_IS_RANGE(w)) return w;
	if (!GTK_IS_CONTAINER(w)) return NULL;
	GtkWidget *r = NULL;
	GList *l = gtk_container_get_children(GTK_CONTAINER(w));
	for (GList *i = l; i && !r; i = i->next)
		r = find_range(GTK_WIDGET(i->data));
	g_list_free(l);
	return r;
}

static int ticks = 0;

static gboolean report(gpointer data) {
	GtkWidget *range = find_range(GTK_WIDGET(data));
	if (range)
		printf("[watch] t=%2d  slider=%.4f\n", ticks, gtk_range_get_value(GTK_RANGE(range)));
	else
		printf("[watch] t=%2d  no GtkRange found\n", ticks);
	fflush(stdout);
	ticks++;
	return G_SOURCE_CONTINUE;
}

static gboolean quit_cb(gpointer data) { (void)data; gtk_main_quit(); return G_SOURCE_REMOVE; }

int main(int argc, char **argv) {
	const char *path = argc > 1 ? argv[1] : "src/.libs/volume_status_menu_item.so";
	gtk_init(&argc, &argv);

	HDPluginModule *m = hd_plugin_module_new(path);
	g_type_module_use(G_TYPE_MODULE(m));
	GObject *obj = hd_plugin_module_new_object(m, "volume");
	if (!obj) { fprintf(stderr, "no object\n"); return 1; }

	GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_default_size(GTK_WINDOW(win), 520, 140);
	gtk_container_add(GTK_CONTAINER(win), GTK_WIDGET(obj));
	g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
	gtk_widget_show_all(win);

	g_timeout_add(500, report, win);
	g_timeout_add(75000, quit_cb, NULL);
	gtk_main();
	return 0;
}
