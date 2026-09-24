/* Reproduce the banner re-show rendering problem while the banner is STILL
 * visible, which is the case that misbehaves on device.
 *
 * A no-window event box draws a bar from a global value. We show it in a
 * banner, then re-show it at short intervals -- while the first banner is
 * still on screen -- changing the value each time.
 *
 * Prints every expose with the value it drew, so a re-show that fails to
 * repaint (or repaints the wrong value) is visible as a missing line. */
#include <hildon/hildon.h>
#include <stdio.h>

static double g_value = 0.10;
static int expose_count = 0;
static int show_count = 0;
static GtkWidget *anchor_widget = NULL;
static GtkWidget *box = NULL;

static gboolean
on_expose(GtkWidget *w, GdkEvent *e, gpointer d)
{
	cairo_t *cr = gdk_cairo_create(GDK_DRAWABLE(w->window));

	expose_count++;
	printf("    EXPOSE #%d  drew value=%.2f  alloc=%dx%d  clip=(%.0f,%.0f %.0fx%.0f)\n",
	       expose_count, g_value,
	       w->allocation.width, w->allocation.height,
	       e->expose.area.x, e->expose.area.y,
	       e->expose.area.width, e->expose.area.height);

	cairo_set_source_rgb(cr, 1, 1, 1);
	cairo_paint(cr);
	cairo_set_source_rgb(cr, 0, 0, 0);
	cairo_rectangle(cr, 10, 20, g_value * 300.0, 18);
	cairo_fill(cr);
	cairo_destroy(cr);
	return FALSE;
}

/* One round: bump the value, queue_draw, re-show. */
static gboolean
round(gpointer data)
{
	int n = GPOINTER_TO_INT(data);
	int before = expose_count;

	g_value = 0.10 * (double)(n + 1);
	show_count++;
	printf("\n-- round %d: value -> %.2f, queue_draw + re-show --\n", n + 1, g_value);

	gtk_widget_queue_draw(box);
	hildon_banner_show_custom_widget(anchor_widget, box);

	/* Check one main-loop iteration later whether a repaint actually landed. */
	printf("    (exposes before=%d)\n", before);
	return FALSE;
}

static gboolean
report(gpointer b)
{
	printf("\n=== %d re-shows, %d exposes total ===\n", show_count, expose_count);
	printf("if exposes < re-shows, some re-shows did not repaint the new value\n");
	gtk_main_quit();
	return FALSE;
}

int main(int argc, char **argv) {
	gtk_init(&argc, &argv);
	setvbuf(stdout, NULL, _IONBF, 0);

	anchor_widget = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_default_size(GTK_WINDOW(anchor_widget), 400, 60);
	gtk_widget_show(anchor_widget);

	box = gtk_event_box_new();
	gtk_event_box_set_visible_window(GTK_EVENT_BOX(box), FALSE);
	gtk_widget_set_size_request(box, 376, 64);
	g_signal_connect(box, "expose-event", G_CALLBACK(on_expose), NULL);

	printf("-- first show at value=0.10 --");
	hildon_banner_show_custom_widget(anchor_widget, box);

	/* Re-show every 150 ms: the banner timeout is several seconds, so every
	 * one of these lands while the previous rendering is still on screen. */
	g_timeout_add(150, round, GINT_TO_POINTER(1));
	g_timeout_add(300, round, GINT_TO_POINTER(2));
	g_timeout_add(450, round, GINT_TO_POINTER(3));
	g_timeout_add(600, round, GINT_TO_POINTER(4));
	g_timeout_add(1500, report, NULL);

	gtk_main();
	return 0;
}
