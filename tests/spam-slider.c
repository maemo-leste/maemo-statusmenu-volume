/* Drive the plugin's slider as fast as possible to force overlapping
 * pa_operations, so the "supersede a still-running operation" path in
 * apply_sink_volume() is actually exercised.
 *
 * Build:
 *   gcc -O0 -g -fsanitize=address -o tests/out/spam-slider tests/spam-slider.c \
 *       $(pkg-config --cflags --libs glib-2.0 libpulse libpulse-mainloop-glib \
 *          gtk+-2.0 libhildondesktop-1 hildon-1 libosso dbus-glib-1 mce x11) -lm
 *
 * Run from src/ with the freshly built plugin, in call mode:
 *   DISPLAY=:0 G_MESSAGES_DEBUG=all ASAN_OPTIONS=detect_leaks=1 \
 *     ../tests/out/spam-slider ./.libs/volume_status_menu_item.so
 */
#include <gtk/gtk.h>
#include <libhildondesktop/libhildondesktop.h>
#include <stdio.h>

static GtkWidget *find_range(GtkWidget *w) {
	GtkWidget *r = NULL;
	GList *l, *i;

	if (!w) return NULL;
	if (GTK_IS_RANGE(w)) return w;
	if (!GTK_IS_CONTAINER(w)) return NULL;

	l = gtk_container_get_children(GTK_CONTAINER(w));
	for (i = l; i && !r; i = i->next)
		r = find_range(GTK_WIDGET(i->data));
	g_list_free(l);
	return r;
}

static int n = 0;
static GtkWidget *g_range = NULL;

static gboolean spam(gpointer d) {
	double v;
	(void) d;

	/* oscillate so every push is a real change */
	v = 0.95 - (n % 40) * 0.02;
	gtk_range_set_value(GTK_RANGE(g_range), v);
	n++;

	if (n >= 200) {
		printf("[spam] finished %d pushes\n", n);
		gtk_main_quit();
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

int main(int argc, char **argv) {
	const char *path = argc > 1 ? argv[1] : ".libs/volume_status_menu_item.so";
	HDPluginModule *m;
	GObject *obj;
	GtkWidget *win;

	gtk_init(&argc, &argv);

	m = hd_plugin_module_new(path);
	if (!m) { fprintf(stderr, "cannot load %s\n", path); return 1; }
	g_type_module_use(G_TYPE_MODULE(m));

	obj = hd_plugin_module_new_object(m, "volume");
	if (!obj) { fprintf(stderr, "no \"volume\" object in %s\n", path); return 1; }

	win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_default_size(GTK_WINDOW(win), 520, 140);
	gtk_container_add(GTK_CONTAINER(win), GTK_WIDGET(obj));
	g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
	gtk_widget_show_all(win);

	g_range = find_range(GTK_WIDGET(obj));
	if (!g_range) { fprintf(stderr, "no GtkRange found in plugin\n"); return 1; }

	printf("[spam] range found, 200 pushes at 1ms\n");
	fflush(stdout);
	g_timeout_add(1, spam, NULL);
	gtk_main();
	return 0;
}
