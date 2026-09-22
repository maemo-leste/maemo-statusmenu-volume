/* Load the volume status-menu plugin, map it, and report what the slider
 * shows compared to the actual sink volume. */
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

static gboolean quit_cb(gpointer data) { (void)data; gtk_main_quit(); return G_SOURCE_REMOVE; }

static gboolean report(gpointer data) {
	GtkWidget *range = find_range(GTK_WIDGET(data));
	if (range)
		printf("[harness] slider value = %.4f\n", gtk_range_get_value(GTK_RANGE(range)));
	else
		printf("[harness] no GtkRange found\n");
	return G_SOURCE_CONTINUE;
}

int main(int argc, char **argv) {
	const char *path = argc > 1 ? argv[1] : ".libs/volume_status_menu_item.so";
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

	for (int i = 1; i <= 12; i++) g_timeout_add(1000 * i, report, win);
	g_timeout_add(13000, quit_cb, NULL);
	gtk_main();
	return 0;
}
