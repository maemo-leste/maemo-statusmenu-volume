/* Characterise hildon_get_dnd() against the canonical contract.
 *
 *   gcc -O0 -g -Isrc -o tests/out/probe-dnd tests/probe-dnd.c \
 *       $(pkg-config --cflags --libs gtk+-2.0 libhildondesktop-1 hildon-1 \
 *          libosso dbus-glib-1 libpulse libpulse-mainloop-glib mce x11) -lm
 *
 * The canonical pair in the stack:
 *
 *   SETTER  hildon_gtk_window_set_clear_window_flag()  [libhildon]
 *             flag TRUE  -> XA_INTEGER, format 32, 1 item, value 1
 *             flag FALSE -> property DELETED
 *           A conforming client can therefore only ever produce "absent"
 *           or "INTEGER/32/1 = 1".
 *
 *   READER  hd_comp_mgr_check_do_not_disturb_flag()    [hildon-desktop]
 *             validates type/format/item count, then requires value == 1
 *
 * This plugin deliberately diverges from that reader and treats mere
 * presence as the flag: for a do-not-disturb atom the conservative
 * failure mode is the one that honours it.  The probe prints both answers
 * side by side so the divergence is visible rather than assumed, and
 * asserts the presence-only result we actually ship.
 *
 * Needs a display.
 */
#include <gtk/gtk.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "item.c"

static GtkWidget *win;
static int failures = 0;

static void
set_prop(Atom type, int format, const void *data, int nitems)
{
	Atom atom = XInternAtom(GDK_DISPLAY(), "_HILDON_DO_NOT_DISTURB", False);

	XChangeProperty(GDK_DISPLAY(), GDK_WINDOW_XID(gtk_widget_get_window(win)),
	                atom, type, format, PropModeReplace,
	                (const unsigned char *) data, nitems);
	XSync(GDK_DISPLAY(), False);
}

static void
clear_prop(void)
{
	Atom atom = XInternAtom(GDK_DISPLAY(), "_HILDON_DO_NOT_DISTURB", False);

	XDeleteProperty(GDK_DISPLAY(), GDK_WINDOW_XID(gtk_widget_get_window(win)),
	                atom);
	XSync(GDK_DISPLAY(), False);
}

/* What hildon-desktop would answer, transcribed from
 * hd_comp_mgr_check_do_not_disturb_flag(). */
static gboolean
canonical_dnd(void)
{
	Atom atom = XInternAtom(GDK_DISPLAY(), "_HILDON_DO_NOT_DISTURB", False);
	guint32 *v = NULL;
	unsigned long n = 0, after = 0;
	int format = 0;
	Atom t = None;
	gboolean ans;

	XGetWindowProperty(GDK_DISPLAY(),
	                   GDK_WINDOW_XID(gtk_widget_get_window(win)),
	                   atom, 0, G_MAXLONG, False, XA_INTEGER,
	                   &t, &format, &n, &after, (unsigned char **) &v);

	ans = (v != NULL && t == XA_INTEGER && format == 32 && n == 1 && v[0] == 1);
	if (v)
		XFree(v);
	return ans;
}

static void
check(const char *label, gboolean want_ours)
{
	gboolean ours = hildon_get_dnd(GDK_WINDOW_XID(gtk_widget_get_window(win)));
	gboolean canon = canonical_dnd();

	printf("  %-42s ours=%-5s  desktop=%-5s  %s%s\n", label,
	       ours ? "TRUE" : "FALSE", canon ? "TRUE" : "FALSE",
	       ours == want_ours ? "OK" : "MISMATCH",
	       (ours != canon) ? "   [diverges from desktop, by design]" : "");
	if (ours != want_ours)
		failures++;
	clear_prop();
}

int main(int argc, char **argv)
{
	guint32 one = 1, zero = 0, two = 2, pair[2] = { 1, 1 };
	guint8 byte_one = 1;
	char *str = "yes";

	gtk_init(&argc, &argv);
	win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_widget_realize(win);

	printf("-- hildon_get_dnd(): ours (presence) vs hildon-desktop (strict) --\n\n");

	/* Everything a conforming client can produce. Both models agree here,
	 * which is why the original code was never actually broken. */
	check("absent", FALSE);
	set_prop(XA_INTEGER, 32, &one, 1);
	check("INTEGER/32 x1 value=1  (canonical setter)", TRUE);

	/* Only reachable from a non-conforming client. We fail toward
	 * honouring the flag; the desktop would not. */
	set_prop(XA_INTEGER, 32, &zero, 1);
	check("INTEGER/32 x1 value=0", TRUE);

	set_prop(XA_INTEGER, 32, &two, 1);
	check("INTEGER/32 x1 value=2", TRUE);

	set_prop(XA_INTEGER, 32, pair, 2);
	check("INTEGER/32 x2", TRUE);

	set_prop(XA_INTEGER, 8, &byte_one, 1);
	check("INTEGER/8 x1 value=1", TRUE);

	set_prop(XA_CARDINAL, 32, &one, 1);
	check("CARDINAL/32 x1 value=1", TRUE);

	set_prop(XA_STRING, 8, str, 3);
	check("STRING/8", TRUE);

	printf("\n  %s\n", failures ? "FAILURES PRESENT" : "ours matches the shipped presence-only contract");
	printf("  note: the XA_INTEGER type filter does not reject the CARDINAL/STRING\n");
	printf("        cases -- X still returns Success with a non-NULL pointer and\n");
	printf("        nitems == 0, so they reach the \"is set\" branch.\n");
	return failures ? 1 : 0;
}
