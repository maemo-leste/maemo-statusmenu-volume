#include <dbus/dbus-glib-lowlevel.h>
#include <gconf/gconf-client.h>
#include <gdk/gdkx.h>
#include <hildon/hildon.h>
#include <libhildondesktop/libhildondesktop.h>
#include <mce/dbus-names.h>
#include <mce/mode-names.h>
#include <pulse/context.h>
#include <pulse/error.h>
#include <pulse/channelmap.h>
#include <pulse/volume.h>
#include <pulse/ext-stream-restore.h>
#include <pulse/glib-mainloop.h>
#include <pulse/introspect.h>
#include <pulse/subscribe.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/XF86keysym.h>
#include <linux/input.h>

#include <math.h>
#include <string.h>

#define CONFIGURATION_FILE "/usr/share/maemo-statusmenu-volume/sinks.ini"

#define SOUND_STATUS_MENU_TYPE_ITEM (sounds_status_menu_item_get_type())
#define SOUND_STATUS_MENU_ITEM(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), \
  SOUND_STATUS_MENU_TYPE_ITEM, SoundsStatusMenuItem))

#if !GLIB_CHECK_VERSION(2, 38, 0)
#define SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item) \
  G_TYPE_INSTANCE_GET_PRIVATE(menu_item, SOUND_STATUS_MENU_TYPE_ITEM, SoundsStatusMenuItemPrivate)
#else
#define SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item) \
  ((SoundsStatusMenuItemPrivate *)sounds_status_menu_item_get_instance_private(menu_item))
#endif

#define DBUS_MCE_MATCH_RULE \
  "type='signal'," \
  "interface='" MCE_SIGNAL_IF "'," \
  "member='" MCE_CALL_STATE_SIG "'"

#define X_KEYCODE_DOWN (XKeysymToKeycode(GDK_DISPLAY(), XF86XK_AudioLowerVolume))
#define X_KEYCODE_UP (XKeysymToKeycode(GDK_DISPLAY(), XF86XK_AudioRaiseVolume))
  
#define HW_KEYCODE_DOWN KEY_VOLUMEDOWN
#define HW_KEYCODE_UP KEY_VOLUMEUP


typedef struct _SoundsStatusMenuItem SoundsStatusMenuItem;
typedef struct _SoundsStatusMenuItemClass SoundsStatusMenuItemClass;
typedef struct _SoundsStatusMenuItemPrivate SoundsStatusMenuItemPrivate;

struct _SoundsStatusMenuItemPrivate
{
  DBusGConnection *dbus;
  GtkWidget *hscale;
  GtkWidget *image;
  gulong hscale_value_changed_id;
  gchar *default_sink_name;
  pa_context *pa_context;
  int normal_volume;
  gboolean normal_volume_set;
  gboolean slider_changed;
  gdouble range_val;
  int call_volume;
  gboolean call_volume_set;
  gboolean call_active;
  gboolean portrait;
  guint8 normal_channels;
  gchar *normal_sink_name;
  gchar *incall_sink_property;
  gint normal_volume_num_steps;
  gint incall_volume_num_steps;
  gint *normal_volume_steps;
  gint *incall_volume_steps;
  GQuark quark_normal;
  GQuark quark_incall;
  pa_mainloop_api *pa_api;
  pa_operation *pa_operation;
  gboolean parent_signals_connected;
  gboolean parent_window_mapped;
  guint mm_key;
  gboolean volume_changed;
  gulong size_changed_id;
  GtkWidget *event_box;
  GdkPixbuf *icon;
  gboolean swap_on_rotate;
  gboolean native_landscape;
  gboolean display_on;
  gboolean keys_are_grabed;
};

struct _SoundsStatusMenuItem
{
  HDStatusMenuItem parent;
};

struct _SoundsStatusMenuItemClass
{
  HDStatusMenuItemClass parent;
};

#if !GLIB_CHECK_VERSION(2, 38, 0)
HD_DEFINE_PLUGIN_MODULE(
    SoundsStatusMenuItem, sounds_status_menu_item, HD_TYPE_STATUS_MENU_ITEM)
#else
HD_DEFINE_PLUGIN_MODULE_EXTENDED(SoundsStatusMenuItem,
                                 sounds_status_menu_item,
                                 HD_TYPE_STATUS_MENU_ITEM,
                                 G_ADD_PRIVATE_DYNAMIC(SoundsStatusMenuItem)
                                 , , )
#endif

static void reconnect(SoundsStatusMenuItem *menu_item);
static void prop_sink_info_cb(pa_context *c, const pa_sink_info *i, int eol,
                              void *userdata);
static void set_volume(SoundsStatusMenuItem *menu_item, int volume);

static void
sounds_status_menu_item_class_finalize(SoundsStatusMenuItemClass *klass)
{
}

static void
grab_keys(SoundsStatusMenuItemPrivate *priv)
{
  Window w = GDK_ROOT_WINDOW();

  XGrabKey(GDK_DISPLAY(), X_KEYCODE_UP, AnyModifier, w, True, GrabModeAsync,
           GrabModeAsync);
  XGrabKey(GDK_DISPLAY(), X_KEYCODE_DOWN, AnyModifier, w, True, GrabModeAsync,
           GrabModeAsync);
  priv->keys_are_grabed = TRUE;
}

static void
ungrab_keys(SoundsStatusMenuItemPrivate *priv)
{
  XUngrabKey(GDK_DISPLAY(), X_KEYCODE_UP, AnyModifier, GDK_ROOT_WINDOW());
  XUngrabKey(GDK_DISPLAY(), X_KEYCODE_DOWN, AnyModifier, GDK_ROOT_WINDOW());
  priv->keys_are_grabed = FALSE;
}

static Window
hildon_window_get_active_window()
{
  Atom atom = XInternAtom(GDK_DISPLAY(), "_NET_ACTIVE_WINDOW", False);
  Window *val = NULL;
  unsigned long bytes_after;
  unsigned long n_items;
  int format;
  Atom type;

  if (XGetWindowProperty(GDK_DISPLAY(), GDK_ROOT_WINDOW(), atom, 0,
                         sizeof(Window), False, XA_WINDOW, &type, &format,
                         &n_items, &bytes_after,
                         (unsigned char **)&val) == Success &&
      type == XA_WINDOW && format == 32 && n_items == 1 && val)
  {
    Window rv = *val;
    XFree(val);
    return rv;
  }

  if (val)
    XFree(val);

  return 0;
}

static gboolean
hildon_get_dnd(Window w)
{
  if (w)
  {
    Atom atom = XInternAtom(GDK_DISPLAY(), "_HILDON_DO_NOT_DISTURB", False);
    long *val = NULL;
    unsigned long bytes_after;
    unsigned long n_items;
    int format;
    Atom type;

    XGetWindowProperty(GDK_DISPLAY(), w, atom, 0, 1, False, XA_INTEGER, &type,
                       &format, &n_items, &bytes_after, (unsigned char **)&val);

    if (val)
    {
      XFree(val);
      return TRUE;
    }
  }

  return FALSE;
}

static gboolean
grab_zoom(SoundsStatusMenuItem *menu_item)
{
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);
  Window window = hildon_window_get_active_window();

  if (window)
  {
    Atom atom = XInternAtom(gdk_display, "_HILDON_ZOOM_KEY_ATOM", False);
    long *val = NULL;
    unsigned long bytes_after;
    unsigned long n_items;
    int format;
    Atom type;

    XGetWindowProperty(GDK_DISPLAY(), window, atom, 0, 1, False, XA_INTEGER,
                       &type, &format,&n_items, &bytes_after,
                       (unsigned char **)&val);

    if (val && *val && !priv->parent_window_mapped)
    {
      ungrab_keys(priv);
      XFree(val);
    }
    else
      grab_keys(priv);

    return TRUE;
  }

  grab_keys(priv);

  return FALSE;
}


static double
pa_vol_to_slider(int vol, int *steps, signed int steps_size)
{
  int step;
  int i;
  double step_coeff;
  double rv;

  g_assert(steps);
  g_assert(steps_size > 1);

  if ( *steps >= vol )
    return 0.0;

  if ( vol >= steps[steps_size - 1] )
    return 1.0;

  for (i = 0; i < steps_size; i++)
  {
    step = steps[i + 1];

    if (vol <= step)
      break;
  }

  if (i == steps_size)
    i--;

  step_coeff = 1.0f / (double)(steps_size - 1);

  rv = (i + 1) * step_coeff;

  if (step > vol)
    rv -= (step - vol) * step_coeff / (step - steps[i]);

  return rv;
}

static const char *
get_icon_name(int volume, gint type)
{
  if (volume == 0)
  {
    if (type == 1)
      return "notification_volume_mute";
    else
      return "statusarea_volume_mute";
  }
  else if (volume <= 10)
  {
    if (type == 1)
      return "notification_volumelevel0";
    else
      return "statusarea_volumelevel0";
  }
  else if (volume <= 40)
  {
    if (type == 1)
      return "notification_volumelevel1";
    else
      return "statusarea_volumelevel1";
  }
  else if (volume <= 60)
  {
    if (type == 1)
      return "notification_volumelevel2";
    else
      return "statusarea_volumelevel2";
  }
  else if (volume <= 80)
  {
    if (type == 1)
      return "notification_volumelevel3";
    else
      return "statusarea_volumelevel3";
  }
  else
  {
    if (type == 1)
      return "notification_volumelevel4";
    else
      return "statusarea_volumelevel4";
  }
}

static void
set_volume_icon(SoundsStatusMenuItem *menu_item, double volume)
{
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);

  gtk_image_set_from_icon_name(GTK_IMAGE(priv->image), get_icon_name(volume, 2),
                               GTK_ICON_SIZE_DIALOG);
}

static void
update_slider(SoundsStatusMenuItem *menu_item)
{
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);
  double val;

  if (priv->call_active)
  {
    val = pa_vol_to_slider(priv->call_volume, priv->incall_volume_steps,
                           priv->incall_volume_num_steps);
  }
  else
  {
    val = pa_vol_to_slider(priv->normal_volume, priv->normal_volume_steps,
                           priv->normal_volume_num_steps);
  }

  g_signal_handler_block(priv->hscale, priv->hscale_value_changed_id);
  gtk_range_set_value(GTK_RANGE(priv->hscale), val);
  g_signal_handler_unblock(priv->hscale, priv->hscale_value_changed_id);

  set_volume_icon(menu_item, val * 100.0);
}

static DBusHandlerResult
dbus_filter(DBusConnection *connection, DBusMessage *message, void *user_data)
{
  SoundsStatusMenuItem *menu_item = user_data;
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);
  DBusError error = DBUS_ERROR_INIT;

  if (dbus_message_is_signal(message, MCE_SIGNAL_IF, MCE_KEY_SIG))
  {
    guint16 hw_keycode;
    gint32 value;

    if (dbus_message_get_args(message, &error,
                              DBUS_TYPE_UINT16, &hw_keycode,
                              DBUS_TYPE_INT32, &value,
                              DBUS_TYPE_INVALID))
    {
      if (priv->keys_are_grabed &&
        !priv->volume_changed &&
        (hw_keycode == HW_KEYCODE_UP || hw_keycode == HW_KEYCODE_DOWN) &&
        value)
      {
        pa_operation *o;

        priv->mm_key = (hw_keycode == HW_KEYCODE_UP ? X_KEYCODE_UP : X_KEYCODE_DOWN);
        o = pa_context_get_sink_info_by_name(priv->pa_context,
                                             priv->normal_sink_name,
                                             prop_sink_info_cb,
                                             menu_item);

        if (o)
          pa_operation_unref(o);
      }
    }
    else if (error.message)
      g_warning("%s", error.message);
  }
  else if (dbus_message_is_signal(message, MCE_SIGNAL_IF, MCE_CALL_STATE_SIG))
  {
    const gchar *type;
    const gchar *state;

    if (dbus_message_get_args(message, &error,
                              DBUS_TYPE_STRING, &state,
                              DBUS_TYPE_STRING, &type,
                              DBUS_TYPE_INVALID))
    {
      priv->call_active = g_str_equal(state, MCE_CALL_STATE_ACTIVE);
      update_slider(menu_item);
    }
    else if (error.message)
    {
      g_warning("%s", error.message);
    }
  }
  else if (dbus_message_is_signal(message, MCE_SIGNAL_IF, MCE_DISPLAY_SIG))
  {
    const gchar *state;

    if (dbus_message_get_args(message, &error,
                              DBUS_TYPE_STRING, &state,
                              DBUS_TYPE_INVALID))
    {
      priv->display_on = g_str_equal(state, MCE_DISPLAY_ON_STRING);
      update_slider(menu_item);
    }
    else if (error.message)
    {
      g_warning("%s", error.message);
    }
  }

  dbus_error_free(&error);

  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static GdkFilterReturn
gdk_filter_func(GdkXEvent *xevent, GdkEvent *event, gpointer data)
{
  SoundsStatusMenuItem *menu_item = data;
  XEvent *xev = (XEvent *)xevent;

  if (xev->type == PropertyNotify)
    grab_zoom(menu_item);

  return GDK_FILTER_CONTINUE;
}

static void
sounds_status_menu_item_dispose(GObject *object)
{
  SoundsStatusMenuItem *menu_item = SOUND_STATUS_MENU_ITEM(object);
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);
  
  gdk_window_remove_filter(0, gdk_filter_func, menu_item);

  if (priv->pa_context)
  {
    pa_context_set_subscribe_callback(priv->pa_context, NULL, NULL);
    pa_context_set_state_callback(priv->pa_context, NULL, NULL);
    pa_context_disconnect(priv->pa_context);
    pa_context_unref(priv->pa_context);
    priv->pa_context = NULL;
  }

  g_signal_handler_disconnect(gdk_screen_get_default(), priv->size_changed_id);

  dbus_connection_remove_filter(dbus_g_connection_get_connection(priv->dbus),
                                dbus_filter, menu_item);
  if (priv->pa_operation)
  {
    pa_operation_unref(priv->pa_operation);
    priv->pa_operation = NULL;
  }

  if (priv->icon)
  {
    g_object_unref(priv->icon);
    priv->icon = NULL;
  }

  G_OBJECT_CLASS(sounds_status_menu_item_parent_class)->dispose(object);
}

static void
sounds_status_menu_item_class_init(SoundsStatusMenuItemClass *klass)
{
#if !GLIB_CHECK_VERSION(2, 38, 0)
  g_type_class_add_private(klass, sizeof(SoundsStatusMenuItemPrivate));
#endif
  G_OBJECT_CLASS(klass)->dispose = sounds_status_menu_item_dispose;
}

static int
x_error_handler(Display *dpy, XErrorEvent *ev)
{
  return 0;
}

static void
get_sinks(SoundsStatusMenuItemPrivate *priv)
{
  GKeyFile *key_file = g_key_file_new();
  GError *error = NULL;

  if (g_key_file_load_from_file(key_file, CONFIGURATION_FILE, G_KEY_FILE_NONE, NULL) )
  {
    priv->normal_sink_name =
        g_key_file_get_string(key_file, "normal", "sink_name", &error);

    if (error)
    {
      g_warning("unable to get normal->sink_name [%s]", error->message);
      g_error_free(error);
      error = NULL;
    }

    priv->incall_sink_property =
        g_key_file_get_string(key_file, "incall", "sink_property", &error);

    if (error)
    {
      g_warning("unable to get incall->sink_property [%s]", error->message);
      g_error_free(error);
      error = NULL;
    }

    priv->swap_on_rotate = g_key_file_get_boolean(key_file, "behavior", "swap_on_rotate", NULL);
    priv->native_landscape = g_key_file_get_boolean(key_file, "behavior", "native_is_landscape", NULL);
  }

  g_key_file_free(key_file);
}

static void
create_volume_steps(SoundsStatusMenuItemPrivate *priv)
{
  gint i, n;

  n = priv->normal_volume_num_steps = 20;
  priv->normal_volume_steps =  g_new(gint, n);

  for (i = 0; i < n; i++)
    priv->normal_volume_steps[i] = (i * 65536.0f) / n;

  n = priv->incall_volume_num_steps = 10;
  priv->incall_volume_steps = g_new(gint, n);

  for (i = 0; i < n; i++)
    priv->incall_volume_steps[i] = (i * 32768.0f) / n + 32768.0f;
}

static void
screen_size_changed_cb(GdkScreen *screen, gpointer user_data)
{
  SoundsStatusMenuItem *menu_item = SOUND_STATUS_MENU_ITEM(user_data);

  SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item)->portrait =
      gdk_screen_get_height(screen) > gdk_screen_get_width(screen);
}

static gboolean
is_running(SoundsStatusMenuItem *menu_item)
{
  SoundsStatusMenuItemPrivate *priv;

  g_return_val_if_fail(SOUND_STATUS_MENU_ITEM(menu_item), FALSE);

  priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);

  if (priv->pa_operation &&
      pa_operation_get_state(priv->pa_operation) == PA_OPERATION_RUNNING)
  {
    return TRUE;
  }

  return FALSE;
}

static void
ext_stream_restore_read_cb(pa_context *c,
                           const pa_ext_stream_restore_info *info, int eol,
                           void *userdata)
{
  SoundsStatusMenuItem *menu_item = userdata;
  SoundsStatusMenuItemPrivate *priv;

  g_assert(menu_item);

  priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);

  if (eol < 0)
  {
    g_warning("Failed to initialized stream_restore extension: %s",
              pa_strerror(pa_context_errno(c)));
    return;
  }

  if (eol)
    return;

  if (is_running(menu_item))
    return;

  if (priv->pa_operation)
  {
    pa_operation_unref(priv->pa_operation);
    priv->pa_operation = NULL;
  }

  if (priv->call_active &&
      g_str_equal(info->name, "sink-input-by-media-role:phone"))
  {
    priv->call_volume = pa_cvolume_avg(&info->volume);
  }
  else if (g_str_equal(info->name, "sink-input-by-media-role:x-maemo"))
  {
    priv->normal_volume = pa_cvolume_avg(&info->volume);
  }
  else
    return;

  if (priv->parent_window_mapped)
    update_slider(menu_item);
}

static void
pa_ext_stream_restore_subscribe_cb(pa_context *c, void *userdata)
{
  pa_operation *o =
      pa_ext_stream_restore_read(c, ext_stream_restore_read_cb, userdata);

  if (o)
    pa_operation_unref(o);
  else
    g_warning("pa_ext_stream_restore_read() failed");
}

static void
ext_stream_restore_test_cb(pa_context *c, uint32_t version, void *userdata)
{
  SoundsStatusMenuItem *menu_item = userdata;
  pa_operation *o;

  g_assert(menu_item);

  o = pa_ext_stream_restore_read(c, ext_stream_restore_read_cb, menu_item);

  if (o)
  {
    pa_operation_unref(o);
    pa_ext_stream_restore_set_subscribe_cb(c,
                                           pa_ext_stream_restore_subscribe_cb,
                                           menu_item);
    o = pa_ext_stream_restore_subscribe(c, 1, NULL, NULL);

    if (o)
      pa_operation_unref(o);
  }
}

static void
context_subscribe_cb(pa_context *c, pa_subscription_event_type_t t,
                     uint32_t idx, void *userdata)
{
  SoundsStatusMenuItem *menu_item = userdata;
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);
  pa_operation *o;

  if (t == PA_SUBSCRIPTION_EVENT_CHANGE)
  {
    o = pa_context_get_sink_info_by_name(c, priv->normal_sink_name,
                                         prop_sink_info_cb, menu_item);
    pa_operation_unref(o);
  }
}

static void
context_state_callback(pa_context *c, void *userdata)
{
  SoundsStatusMenuItem *menu_item = userdata;
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);
  pa_context_state_t state;

  g_assert(c);
  g_assert(menu_item);

  state = pa_context_get_state(c);

  if (state == PA_CONTEXT_READY || state == PA_CONTEXT_FAILED ||
      state == PA_CONTEXT_TERMINATED)
  {
    if (state == PA_CONTEXT_READY)
    {
      pa_operation *o;
      o = pa_ext_stream_restore_test(c, ext_stream_restore_test_cb, menu_item);

      if (o)
        pa_operation_unref(o);
      else
      {
        g_critical("Failed to initialized stream_restore extension: %s",
                   pa_strerror(pa_context_errno(c)));
      }

      pa_context_set_subscribe_callback(c, context_subscribe_cb, menu_item);
      o = pa_context_subscribe(c, PA_SUBSCRIPTION_MASK_SERVER, 0, menu_item);

      if (o)
        pa_operation_unref(o);

      o = pa_context_get_sink_info_by_name(priv->pa_context, priv->normal_sink_name,
                                       prop_sink_info_cb, menu_item);
      if (o)
        pa_operation_unref(o);
      else
        g_warning("Pulse audio failure: %s %s",
                  pa_strerror(pa_context_errno(priv->pa_context)), priv->normal_sink_name);
    }
    else
      reconnect(menu_item);
  }
}

static void
reconnect(SoundsStatusMenuItem *menu_item)
{
  SoundsStatusMenuItemPrivate *priv;

  g_assert(menu_item);

  priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);

  g_assert(priv);

  if (priv->pa_context)
    pa_context_unref(priv->pa_context);

  priv->pa_context = pa_context_new(priv->pa_api, "maemo-statusmenu-volume");

  pa_context_set_state_callback(priv->pa_context, context_state_callback,
                                menu_item);

  if (pa_context_connect(priv->pa_context, NULL,
                         PA_CONTEXT_NOFAIL | PA_CONTEXT_NOAUTOSPAWN, NULL) < 0)
  {
    g_warning("Failed to connect pa server: %s",
              pa_strerror(pa_context_errno(priv->pa_context)));
  }
}

static gint
slider_to_pa_vol(double volume, gint *steps, gint num_steps)
{
  double vol_dbl;
  int step, steps_minus_one;
  int pa_vol;

  g_assert(steps);
  g_assert(num_steps > 1);

  steps_minus_one = num_steps - 1;

  vol_dbl = (double)steps_minus_one * volume;
  step = floor(vol_dbl);

  if (steps_minus_one <= step)
    return steps[steps_minus_one];

  if (step <= 0)
    return steps[0];

  pa_vol = steps[step];

  if (vol_dbl - step > 0.0)
    pa_vol += lrint((steps[step + 1] - pa_vol) * (vol_dbl - step));

  return pa_vol;
}

static gboolean
reset_volume_changed(gpointer user_data)
{
  SoundsStatusMenuItem *menu_item = user_data;
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);

  priv->volume_changed = FALSE;

  return FALSE;
}

static double
slider_volume_increase_step(SoundsStatusMenuItem *menu_item, double volume)
{
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);
  gint *steps;
  gint num_steps;
  int pa_vol;
  int i;

  if (priv->call_active)
  {
    steps = priv->incall_volume_steps;
    num_steps = priv->incall_volume_num_steps;
  }
  else
  {
    steps = priv->normal_volume_steps;
    num_steps = priv->normal_volume_num_steps;
  }

  if (num_steps <= 0)
    return 1.0;

  pa_vol = slider_to_pa_vol(volume, steps, num_steps) + 1;

  for (i = 0; i < num_steps - 1; i++)
  {
    if (pa_vol < steps[i])
      break;
  }

  return i / (double)(num_steps - 1);
}

static double
slider_volume_decrease_step(SoundsStatusMenuItem *menu_item, double volume)
{
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);
  gint *steps;
  gint num_steps;
  int pa_vol;
  int i;

  if (priv->call_active)
  {
    steps = priv->incall_volume_steps;
    num_steps = priv->incall_volume_num_steps;
  }
  else
  {
    steps = priv->normal_volume_steps;
    num_steps = priv->normal_volume_num_steps;
  }

  pa_vol = slider_to_pa_vol(volume, steps, num_steps) - 1;

  if (num_steps == 1)
    return 0.0;

  for (i = num_steps - 1; i; i--)
  {
    if (pa_vol > steps[i])
      break;
  }

  return i / (double)(num_steps - 1);
}

static void
draw_volume_bar(cairo_t *cr, double x, double y, double width, double height,
                double radius, gboolean round_right)
{
  double rx = radius;
  double ry = radius;
  double r = radius;

  if (rx > 0.5 * width)
    rx = 0.5 * width;

  if (ry > 0.5 * height)
    ry = 0.5 * height;

  r = fmin(rx, ry);

  cairo_move_to(cr, x + r, y);

  if (round_right)
  {
    cairo_arc(cr, x + width - r, y + r, r, M_PI + M_PI_2, 2.0 * M_PI);
    cairo_arc(cr, x + width - r, y + height - r, r, 0.0, M_PI_2);
  }
  else
  {
    cairo_line_to(cr, x + width, y);
    cairo_line_to(cr, x + width, y + height);
  }

  cairo_arc(cr, x + r, y + height - r, r, M_PI_2, M_PI);
  cairo_arc(cr, x + r, y + r, r, M_PI, M_PI + M_PI_2);
}

static gboolean
expose_event_cb(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  SoundsStatusMenuItem *menu_item = SOUND_STATUS_MENU_ITEM(user_data);
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);
  cairo_t *cr = gdk_cairo_create(GDK_DRAWABLE(widget->window));
  gint scr_w = gdk_screen_get_width(gdk_screen_get_default());
  gdouble volume = gtk_range_get_value(GTK_RANGE(priv->hscale));
  int x = (scr_w - 376) / 2;
  const char *icon_name;

  if (priv->icon)
    g_object_unref(priv->icon);

  icon_name = get_icon_name(volume * 100.0, 1);
  priv->icon = gtk_icon_theme_load_icon(
        gtk_icon_theme_get_default(), icon_name,
        hildon_get_icon_pixel_size(HILDON_ICON_SIZE_FINGER), 0, 0);

  if (priv->icon)
  {
    gdk_cairo_set_source_pixbuf(
          cr, priv->icon, x,
          (widget->allocation.y + (widget->allocation.height -
                                   gdk_pixbuf_get_height(priv->icon)) / 2));
    cairo_paint(cr);
  }


  draw_volume_bar(cr, x + 53, 30.0, volume * 322.0, 18.0, 2, volume > 0.99);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 1.0);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  cairo_fill(cr);

  draw_volume_bar(cr, x + 52, 29.0, 324.0, 20.0, 4, TRUE);
  cairo_set_line_width(cr, 2.0);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.5);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  cairo_stroke(cr);
  cairo_destroy(cr);

  return FALSE;
}

static gboolean
parse_tuning_property(const gchar *property, gint *num_steps_out,
                      gint **steps_out, GQuark *quark)
{
  const gchar *p;
  gchar **steps_array;
  gint i, l;
  gint num_steps = 1;
  gint *steps;
  GQuark q = g_quark_from_string(property);

  if (q == *quark )
    return FALSE;

  *quark = q;
  p = strchr(property, '=');

  if (!p)
    p = property;

  steps_array = g_strsplit(p, ",", -1);
  l = g_strv_length(steps_array);
  steps = g_new(gint, l + 2);
  steps[0] = 0;

  for (i = 0; i < l; i++)
  {
    gchar **step_array = g_strsplit(steps_array[i], ":", -1);
    int vol;

    if (g_strv_length(step_array) != 2 || !step_array[1] || !*step_array[1])
      continue;

    vol = strtol(step_array[1], NULL, 10);

    if (i == 0 && vol <= -6000)
    {
      g_strfreev(step_array);
      continue;
    }

    steps[num_steps++] = pa_sw_volume_from_dB(vol / 100.0);
    g_strfreev(step_array);
  }

  g_strfreev(steps_array);

  *num_steps_out = num_steps;
  g_free(*steps_out);
  *steps_out = (gint *)g_realloc(steps, num_steps * sizeof(steps[0]));

  return TRUE;
}

static
void error_callback(pa_context *c, int success, void *userdata)
{
    if (!success) 
        g_warning("Pulse audio failure: %s", pa_strerror(pa_context_errno(c)));
}

static void
prop_sink_info_cb(pa_context *c, const pa_sink_info *i, int eol, void *userdata)
{
  SoundsStatusMenuItem *menu_item = userdata;
  SoundsStatusMenuItemPrivate *priv;
  gboolean volume_changed = FALSE;
  const char *prop_incall;
  gint *volume;
  gint steps_size;
  gint *steps;
  double current_vol;
  double new_vol;
  
  g_assert((priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item)));
  
  if (i && priv->normal_channels == 0 && g_str_equal(priv->normal_sink_name, i->name))
    priv->normal_channels = i->channel_map.channels;

  if (eol)
    return;

  prop_incall = pa_proplist_gets(i->proplist, priv->incall_sink_property);

  parse_tuning_property(prop_incall,&priv->incall_volume_num_steps,
                        &priv->incall_volume_steps, &priv->quark_incall);

  if (!priv->mm_key)
    goto out;

  if (priv->call_active)
  {
    volume = &priv->call_volume;
    steps = priv->incall_volume_steps;
    steps_size = priv->incall_volume_num_steps;
  }
  else
  {
    volume = &priv->normal_volume;
    steps = priv->normal_volume_steps;
    steps_size = priv->normal_volume_num_steps;
  }

  current_vol = pa_vol_to_slider(*volume, steps, steps_size);
  
  if ((!priv->portrait && priv->swap_on_rotate && priv->display_on) ||
    (!priv->display_on && priv->native_landscape))
  {
    if (priv->mm_key == X_KEYCODE_UP)
      priv->mm_key = X_KEYCODE_DOWN;
    else if (priv->mm_key == X_KEYCODE_DOWN)
      priv->mm_key = X_KEYCODE_UP;
  }

  if (priv->mm_key == X_KEYCODE_UP)
  {
    new_vol = slider_volume_increase_step(menu_item, current_vol);
    volume_changed = TRUE;
  }
  else if (priv->mm_key == X_KEYCODE_DOWN)
  {
    new_vol = slider_volume_decrease_step(menu_item, current_vol);
    volume_changed = TRUE;
  }

  if (volume_changed)
  {
    set_volume(menu_item, slider_to_pa_vol(new_vol, steps, steps_size));
    priv->volume_changed = TRUE;
    g_timeout_add(50, reset_volume_changed, menu_item);

    /* FIXME - why calling update_slider twice */
    if (priv->parent_window_mapped)
      update_slider(menu_item);
    else if (!hildon_get_dnd(hildon_window_get_active_window()))
    {
      update_slider(menu_item);

      if (!priv->event_box)
      {
        GtkWidget *evt_box = gtk_event_box_new();

        gtk_event_box_set_visible_window(GTK_EVENT_BOX(evt_box), False);
        gtk_widget_set_size_request(evt_box, 472, 64);
        g_signal_connect(evt_box, "expose-event", G_CALLBACK(expose_event_cb),
                         menu_item);
        priv->event_box = evt_box;
        g_signal_connect(evt_box, "destroy", G_CALLBACK(gtk_widget_destroyed),
                         &priv->event_box);
      }

      gtk_widget_queue_draw(priv->event_box);
      hildon_banner_show_custom_widget(GTK_WIDGET(menu_item), priv->event_box);
    }
  }

out:
  priv->mm_key = 0;

  if (priv->parent_window_mapped)
    update_slider(menu_item);
}

static void
set_volume(SoundsStatusMenuItem *menu_item, int volume)
{
  SoundsStatusMenuItemPrivate *priv;

  g_return_if_fail(menu_item);

  priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);

  if (priv->call_active)
  {
    priv->call_volume = volume;
    priv->call_volume_set = TRUE;
  }
  else
  {
    pa_cvolume cv;
    priv->normal_volume = volume;
    priv->normal_volume_set = TRUE;
    for (guint8 i = 0; i < priv->normal_channels; ++i)
      cv.values[i] = volume;
    cv.channels = priv->normal_channels;
    pa_operation *o;

    o = pa_context_set_sink_volume_by_name(priv->pa_context, priv->normal_sink_name, &cv, error_callback, NULL);
    if (o)
      pa_operation_unref(o);
    else 
      g_warning("Pulse audio failure: %s %s",
                pa_strerror(pa_context_errno(priv->pa_context)), priv->normal_sink_name);
  }
}

static void
hscale_value_changed_cb(GtkRange *range, gpointer user_data)
{
  SoundsStatusMenuItem *menu_item = (SoundsStatusMenuItem *)user_data;
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);
  int pa_vol;

  priv->range_val = gtk_range_get_value(GTK_RANGE(range));

  if (priv->call_active)
  {
    pa_vol = slider_to_pa_vol(priv->range_val, priv->incall_volume_steps,
                              priv->incall_volume_num_steps);
  }
  else
  {
    pa_vol = slider_to_pa_vol(priv->range_val, priv->normal_volume_steps,
                              priv->normal_volume_num_steps);
  }

  priv->slider_changed = TRUE;
  set_volume(menu_item, pa_vol);
  set_volume_icon(menu_item, gtk_range_get_value(GTK_RANGE(range)) * 100.0f);
}

static void
parent_window_map_cb(GtkWidget *widget, SoundsStatusMenuItem *menu_item)
{
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);
  pa_operation *o;

  grab_keys(priv);

  o = pa_context_subscribe(
        priv->pa_context,
        PA_SUBSCRIPTION_MASK_SERVER | PA_SUBSCRIPTION_MASK_SINK, 0, menu_item);

  if (o)
    pa_operation_unref(o);

  o = pa_context_get_sink_info_by_name(priv->pa_context, priv->normal_sink_name,
                                       prop_sink_info_cb, menu_item);

  if (o)
    pa_operation_unref(o);

  priv->parent_window_mapped = TRUE;
  update_slider(menu_item);
}

static void
parent_window_unmap_cb(GtkWidget *widget, SoundsStatusMenuItem *menu_item)
{
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);
  pa_operation *o = pa_context_subscribe(
        priv->pa_context, PA_SUBSCRIPTION_MASK_SERVER, 0, menu_item);
  gboolean ret;

  if (o)
    pa_operation_unref(o);

  priv->parent_window_mapped = FALSE;
  g_signal_emit_by_name(priv->hscale, "grab-broken-event", NULL, &ret);
  grab_zoom(menu_item);
}

static void
parent_set_cb(GtkWidget *widget, GtkWidget *old_parent,
              SoundsStatusMenuItem *menu_item)
{
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);
  GtkWidget *ancestor;

  if (priv->parent_signals_connected)
  {
    ancestor = gtk_widget_get_ancestor(GTK_WIDGET(old_parent), GTK_TYPE_WINDOW);
    g_signal_handlers_disconnect_matched(
          G_OBJECT(ancestor), G_SIGNAL_MATCH_DATA | G_SIGNAL_MATCH_FUNC, 0, 0,
          NULL, parent_window_map_cb, menu_item);
    g_signal_handlers_disconnect_matched(
          G_OBJECT(ancestor), G_SIGNAL_MATCH_DATA | G_SIGNAL_MATCH_FUNC, 0, 0,
          NULL, parent_window_unmap_cb, menu_item);
    priv->parent_signals_connected = FALSE;
  }

  ancestor = gtk_widget_get_ancestor(GTK_WIDGET(menu_item), GTK_TYPE_WINDOW);

  if (ancestor)
  {
    g_signal_connect(G_OBJECT(ancestor), "map",
                     G_CALLBACK(parent_window_map_cb), menu_item);
    g_signal_connect(G_OBJECT(ancestor), "unmap",
                     G_CALLBACK(parent_window_unmap_cb), menu_item);
    priv->parent_signals_connected = TRUE;
  }
}

static void
sounds_status_menu_item_init(SoundsStatusMenuItem *menu_item)
{
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);
  pa_glib_mainloop *m;
  GtkWidget *hbox;
  DBusConnection *conn ;
  GError *error = NULL;
  GdkScreen *screen;

  XSetErrorHandler(x_error_handler);

  priv->volume_changed = FALSE;
  priv->call_active = FALSE;
  priv->pa_context = NULL;
  priv->pa_operation = NULL;
  priv->parent_signals_connected = FALSE;
  priv->parent_window_mapped = FALSE;
  priv->mm_key = 0;
  priv->icon = NULL;
  priv->normal_channels = 0;
  priv->swap_on_rotate = FALSE;
  priv->display_on = TRUE;
  priv->keys_are_grabed = FALSE;
  priv->native_landscape = FALSE;

  get_sinks(priv);
  create_volume_steps(priv);
  grab_keys(priv);

  gdk_window_set_events(GDK_ROOT_PARENT(),
                        gdk_window_get_events(GDK_ROOT_PARENT()) |
                        GDK_PROPERTY_CHANGE_MASK);
  
  gdk_window_add_filter(0, gdk_filter_func, menu_item);

  screen = gdk_screen_get_default();
  priv->portrait = gdk_screen_get_height(screen) > gdk_screen_get_width(screen);
  priv->size_changed_id = g_signal_connect(
        screen, "size-changed", G_CALLBACK(screen_size_changed_cb), menu_item);

  m = pa_glib_mainloop_new(g_main_context_default());
  g_assert(m);

  priv->pa_api = pa_glib_mainloop_get_api(m);
  reconnect(menu_item);

  priv->dbus = dbus_g_bus_get(DBUS_BUS_SYSTEM, &error);

  if (!priv->dbus)
  {
    g_warning("Failed to open connection to bus: %s", error->message);
    return;
  }

  conn = dbus_g_connection_get_connection(priv->dbus);

  dbus_bus_add_match(conn, DBUS_MCE_MATCH_RULE, NULL);
  dbus_connection_add_filter(conn, dbus_filter, menu_item, NULL);

  hbox = gtk_hbox_new(FALSE, 0);

  priv->image = gtk_image_new();
  gtk_box_pack_start(GTK_BOX(hbox), priv->image, FALSE, FALSE, 8);

  priv->hscale = hildon_gtk_hscale_new();
  gtk_box_pack_start(GTK_BOX(hbox), priv->hscale, TRUE, TRUE, 8);
  priv->hscale_value_changed_id  =
      g_signal_connect(priv->hscale, "value-changed",
                       G_CALLBACK(hscale_value_changed_cb), menu_item);

  gtk_widget_show(priv->hscale);
  gtk_widget_show(priv->image);
  gtk_widget_show(hbox);

  g_signal_connect(G_OBJECT(menu_item), "parent-set",
                   G_CALLBACK(parent_set_cb), menu_item);
  gtk_container_add(GTK_CONTAINER(menu_item), hbox);
  gtk_widget_show(GTK_WIDGET(menu_item));
}
