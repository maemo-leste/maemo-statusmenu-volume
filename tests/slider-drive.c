/* Drive the plugin's slider through a fixed schedule so the shell can sample
 * the resulting sink volume between steps.
 *
 *   gcc -O0 -g -o tests/out/slider-drive tests/slider-drive.c \
 *       $(pkg-config --cflags --libs gtk+-2.0 libhildondesktop-1)
 *
 * Each step is STEP_MS apart; the harness prints "SET <value>" when it
 * applies one.  Sample the sink at ~2/3 of the interval.
 */
#include <gtk/gtk.h>
#include <libhildondesktop/libhildondesktop.h>
#include <stdio.h>
#include <stdlib.h>

#define STEP_MS 3000

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

static double *vals;
static int n_vals, idx;
static GtkWidget *range;

static gboolean step(gpointer data) {
	(void)data;
	if (idx >= n_vals) { gtk_main_quit(); return G_SOURCE_REMOVE; }
	gtk_range_set_value(GTK_RANGE(range), vals[idx]);
	printf("SET %.2f\n", vals[idx]);
	fflush(stdout);
	idx++;
	return G_SOURCE_CONTINUE;
}

static gboolean quit_cb(gpointer d) { (void)d; gtk_main_quit(); return G_SOURCE_REMOVE; }

int main(int argc, char **argv) {
	gtk_init(&argc, &argv);
	if (argc < 3) { fprintf(stderr, "usage: %s <plugin.so> <val> [val...]\n", argv[0]); return 1; }

	HDPluginModule *m = hd_plugin_module_new(argv[1]);
	g_type_module_use(G_TYPE_MODULE(m));
	GObject *obj = hd_plugin_module_new_object(m, "volume");
	if (!obj) { fprintf(stderr, "no object\n"); return 1; }

	GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_default_size(GTK_WINDOW(win), 520, 140);
	gtk_container_add(GTK_CONTAINER(win), GTK_WIDGET(obj));
	g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
	gtk_widget_show_all(win);

	range = find_range(GTK_WIDGET(obj));
	if (!range) { fprintf(stderr, "no GtkRange\n"); return 1; }

	n_vals = argc - 2;
	vals = g_new(double, n_vals);
	for (int i = 0; i < n_vals; i++)
		vals[i] = g_ascii_strtod(argv[2 + i], NULL);

	g_timeout_add(STEP_MS, step, NULL);
	g_timeout_add(STEP_MS * (n_vals + 1), quit_cb, NULL);
	gtk_main();
	return 0;
}
