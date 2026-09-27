#include <X11/XF86keysym.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <dbus/dbus-glib-lowlevel.h>
#include <gdk/gdkx.h>
#include <hildon/hildon.h>
#include <libhildondesktop/libhildondesktop.h>
#include <linux/input.h>
#include <mce/dbus-names.h>
#include <mce/mode-names.h>
#include <pipewire/extensions/metadata.h>
#include <pipewire/pipewire.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/pod/pod.h>
#include <spa/pod/vararg.h>
#include <spa/param/audio/raw.h>
#include <spa/param/props.h>
#include <spa/utils/defs.h>
#include <spa/utils/json.h>

#include <math.h>
#include <string.h>

#define CONFIGURATION_FILE "/usr/share/maemo-statusmenu-volume/sinks.ini"

/* No tuning table is compiled in. Every ladder this applet uses comes
 * from the sink node it is bound to, published by the audio stack (the
 * wireplumber rules in maemo-audio). A second copy here would be a
 * second thing to keep in step, and a curve that silently disagrees with
 * the hardware is worse than no curve at all.
 *
 * Until a ladder has been read there is nothing to map against, so the
 * slider is insensitive and the volume keys are ignored. See
 * have_ladder(). */

/* The keys the audio stack publishes the ladders under, and the ones
 * this applet reads. sinks.ini can name different ones; these are the
 * default it falls back to so an absent override does not read as "no
 * ladder". */
#define TUNING_KEY         "x-maemo.volume.tuning"
#define TUNING_INCALL_KEY  "x-maemo.volume.tuning.incall"

/* No volume ramp here.  The ramp in audioconvert is driven by the node's
 * master `volume` (prev_volume -> volume), and the level this applet owns
 * lives in channelVolumes, so asking for a ramp would leave `volume`
 * untouched and make audioconvert log "no change in volume, cannot ramp
 * volume" on every write.  A hard step is also exactly what the
 * PulseAudio path did. */

/* How long to sit out before retrying a failed PipeWire connection. */
#define RECONNECT_DELAY_SECONDS 2

/* Volume intent, written on the tracked sink node and read back for the
 * slider.
 *
 * The applet does not set the sink volume.  A sink's level is owned by the
 * card route: pipewire-pulse writes it with pw_device_set_param(SPA_PARAM_
 * Route, save=true) and reads it back out of that same route, so a bare
 * node Props write leaves every PulseAudio client quoting a stale number
 * and skips the per-port persistence the route store provides.  Writing a
 * target instead keeps one writer for the sink -- the policy -- and leaves
 * this applet holding only what the user asked for. */
#define TARGET_KEY "x-maemo.volume.target"

/* Tuning centibels to the internal step scale.
 *
 * This is the perceptual (cubic) mapping, not the linear one: 60 dB per
 * decade, so 0 dB lands on the norm and -60 dB lands on a tenth of it.
 * 65536 is the same anchor the tables have always produced, which keeps
 * every stored ladder reading the same as before.
 *
 * Verified bit-for-bit against pa_sw_volume_from_dB() over -8000..+1200 cB
 * (9201 values, zero differences).  Note that lrint() is what matches:
 * truncating disagrees with PulseAudio on about half of all values, which
 * would silently shift every ladder by a step. */
#define VOLUME_NORM 65536.0

static gint
tuning_cb_to_volume(gint centibel)
{
  double db = (double)centibel / 100.0;

  return (gint)lrint(VOLUME_NORM * pow(10.0, db / 60.0));
}

/* PipeWire carries volume as linear amplitude; the tables, the ladder math
 * and every cached level in here are on the perceptual (cubic) scale where
 * 65536 is 0 dB and -60 dB is 6554.  These two are the only place the two
 * scales meet, and they are the exact inverse of pipewire-pulse's own
 * volume_to_linear() / volume_from_linear() in module-protocol-pulse, so a
 * level written here lands where the PulseAudio path would have put it.
 *
 * Note this is a cube, not a divide-by-65536.  Treating the step scale as
 * linear puts every level somewhere else entirely: the sink's 13107
 * (-41.94 dB) is 0.008 linear, not 0.2. */
static inline gint
spa_volume_to_step(float v)
{
  if (v <= 0.0f)
    return 0;

  return (gint)lrint(cbrt((double) v) * VOLUME_NORM);
}

static inline float
step_to_spa_volume(gint step)
{
  double n = (double) step / VOLUME_NORM;

  return (float)(n * n * n);
}

/* Pull the level out of a SPA_PARAM_Props pod.
 *
 * channelVolumes, not volume.  The node's `volume` is a master gain that
 * multiplies on top of the per-channel values (channelmix_set_volume in
 * audioconvert) and pipewire-pulse never touches it:
 * set_node_volume_mute() writes SPA_PROP_channelVolumes, and that is what
 * pactl reports and what wireplumber persists for the route.  Reading
 * `volume` here would pin the slider at 0 dB no matter where the sink
 * actually is. */
static gboolean
read_pod_volume(const struct spa_pod *param, gint *volume_out)
{
  const struct spa_pod_object *obj = (const struct spa_pod_object *) param;
  const struct spa_pod_prop *prop;
  float lin[1] = { 0.0f };

  if (!param)
    return FALSE;

  prop = spa_pod_object_find_prop(obj, NULL, SPA_PROP_channelVolumes);
  if (!prop)
    return FALSE;

  /* An array, not &scalar: the macro expands sizeof(values[0]) without
   * parenthesising, so &lin[0] reads as &(lin[0]) and subscripts a
   * float. */
  if (spa_pod_copy_array(&prop->value, SPA_TYPE_Float, lin, 1) < 1)
    return FALSE;

  *volume_out = spa_volume_to_step(lin[0]);
  return TRUE;
}

/* Build the Props pod that sets every channel to one level, which is what
 * pa_cvolume_set() did on the PulseAudio side.  Balance is not preserved,
 * same as before -- the applet has never had a balance control. */
static struct spa_pod *
build_volume_pod(struct spa_pod_builder *b, gint volume, uint32_t n_channels)
{
  float vals[SPA_AUDIO_MAX_CHANNELS];
  float lin = step_to_spa_volume(volume);
  uint32_t i;

  for (i = 0; i < n_channels; i++)
    vals[i] = lin;

  return spa_pod_builder_add_object(b,
           SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
           SPA_PROP_channelVolumes,
             SPA_POD_Array(sizeof(float), SPA_TYPE_Float, n_channels, vals));
}

#define SOUND_STATUS_MENU_TYPE_ITEM (sounds_status_menu_item_get_type())
#define SOUND_STATUS_MENU_ITEM(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), \
                              SOUND_STATUS_MENU_TYPE_ITEM, SoundsStatusMenuItem))

#if !GLIB_CHECK_VERSION(2, 38, 0)
#define SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item) \
  G_TYPE_INSTANCE_GET_PRIVATE(menu_item, \
                              SOUND_STATUS_MENU_TYPE_ITEM, \
                              SoundsStatusMenuItemPrivate)
#else
#define SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item) \
  ((SoundsStatusMenuItemPrivate *)sounds_status_menu_item_get_instance_private( \
     menu_item))
#endif

/* Every MCE signal this plugin consumes gets its own match rule.
 *
 * We deliberately do not rely on the broad "interface='com.nokia.mce.signal'"
 * match that our host process (hildon-status-menu, see hd-display.c) installs.
 * dbus_bus_get() and dbus_g_bus_get() return the same process-wide shared
 * DBusConnection, so that broad match also feeds our filter and the volume
 * keys appear to work even without a rule of our own.  Depending on it would
 * mean a host narrowing its match rules silently disables volume key handling
 * here.  The bus broker refcounts duplicate rules, so matching again is free.
 *
 * sig_call_state_ind is deliberately absent: the in-call state now comes from
 * the sink input list (see update_call_state), which is queryable rather than
 * edge-triggered, so a start-up during a call sees the truth instead of
 * having missed a transition.
 */
#define DBUS_MCE_KEY_MATCH_RULE \
  "type='signal'," \
  "interface='" MCE_SIGNAL_IF "'," \
  "member='" MCE_KEY_SIG "'"

#define DBUS_MCE_DISPLAY_MATCH_RULE \
  "type='signal'," \
  "interface='" MCE_SIGNAL_IF "'," \
  "member='" MCE_DISPLAY_SIG "'"

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
  /* Transport.  PipeWire owns its own loop but not its own thread: the
   * loop's epoll fd is watched by GLib and drained with
   * pw_loop_iterate(loop, false), so registry callbacks, the X key grabs
   * and the MCE D-Bus filter all run on the one GTK thread and no
   * marshalling between event sources is needed. */
  struct pw_main_loop *pw_loop;
  GSource *pw_bridge;
  GIOChannel *pw_channel;
  struct pw_context *pw_context;
  struct pw_core *pw_core;
  struct spa_hook pw_core_hook;
  struct pw_registry *pw_registry;
  struct spa_hook pw_registry_hook;
  struct pw_metadata *pw_metadata;
  struct spa_hook pw_metadata_hook;
  /* Proxy for the sink we track, plus its id so a registry removal can be
   * recognised.  have_sink_props / have_sink_volume say which halves of
   * the picture have landed: properties arrive on node.info, the level on
   * the SPA_PARAM_Props param, and the slider needs both. */
  struct pw_proxy *sink_proxy;
  struct spa_hook sink_hook;
  uint32_t sink_id;
  gboolean have_sink_props;
  gboolean have_sink_volume;
  /* The user's target for this output, and what the slider draws.  Kept
   * apart from `volume` because during a call the slider rides the call
   * stream while the media target stays put underneath it. */
  gint target;
  gboolean have_target;

  /* A level that arrived before the ladder to map it onto. The ladder
   * and the level come on separate channels and either can win the race;
   * rather than drop the level or re-run the sync on every unrelated
   * event, the request is parked here and replayed when the ladder is
   * installed. */
  gboolean deferred_state_sync;
  /* Last value seen for the default.audio.sink metadata entry, kept so a
   * sink removal can re-point without another round trip. */
  gchar *default_sink_name;
  /* Every sink the registry has announced, name -> node id.  A global is
   * announced once and cannot be re-requested, so the map is what lets a
   * later resolve_sink() find a sink that showed up before we knew its
   * name. */
  GHashTable *sink_ids;
  guint32 sync_seq;
  guint reconnect_id;
  /* The level of the sink we track, straight off SPA_PARAM_Props.  Kept
   * apart from `volume` because during a call the slider rides the call
   * stream and a sink event must not overwrite that. */
  gint sink_volume;
  /* Level the slider shows, and the level writes are made from. */
  int volume;
  gdouble range_val;
  /* TRUE while at least one sink input carries media.role=phone.  Replaces
   * the MCE sig_call_state_ind edge signal: the stream list is queryable at
   * any moment, so an applet that starts mid-call sees the truth instead of
   * missing a transition it was not listening for. */
  gboolean call_active;
  /* Accumulator for the in-progress sink input scan. */
  gboolean call_active_pending;
  /* Every live call-role stream, keyed by node id.  While a call is
   * active the slider reads from and writes to these instead of the sink,
   * which is what keeps media and VOIP volume independent. */
  GHashTable *call_streams;
  gboolean portrait;
  guint8 normal_channels;
  gchar *normal_sink_name;
  gboolean normal_sink_name_provided;
  gchar *normal_sink_property;
  gchar *incall_sink_property;
  gint normal_volume_num_steps;
  gint incall_volume_num_steps;
  gint *normal_volume_steps;
  gint *incall_volume_steps;
  GQuark quark_normal;
  GQuark quark_incall;
  gboolean warned_normal_tuning;
  gboolean warned_incall_tuning;
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
  gboolean keys_are_grabbed;
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

static void
reconnect(SoundsStatusMenuItem *menu_item);
static void
schedule_reconnect(SoundsStatusMenuItem *menu_item);
static void
read_sink_volume(SoundsStatusMenuItem *menu_item);
static void
sink_state_changed(SoundsStatusMenuItem *menu_item, const char *why);
static void
bind_sink(SoundsStatusMenuItem *menu_item, uint32_t id);
static void
write_target(SoundsStatusMenuItem *menu_item, gint volume);
static void
clear_sink(SoundsStatusMenuItem *menu_item);
static void
follow_default_sink(SoundsStatusMenuItem *menu_item);
static void
resolve_sink(SoundsStatusMenuItem *menu_item);
static void
teardown_connection(SoundsStatusMenuItem *menu_item);
static void
call_state_changed(SoundsStatusMenuItem *menu_item);
static void
set_volume(SoundsStatusMenuItem *menu_item, int volume);
static gboolean
parse_tuning_property(const gchar *property, gint *num_steps_out,
                      gint **steps_out, GQuark *quark);
static void
clear_tuning(gint *num_steps_out, gint **steps_out, GQuark *quark);
static gboolean
have_ladder(SoundsStatusMenuItemPrivate *priv);
static void
sync_slider_sensitivity(SoundsStatusMenuItem *menu_item);

static void
sounds_status_menu_item_class_finalize(SoundsStatusMenuItemClass *klass)
{}

static void
grab_keys(SoundsStatusMenuItemPrivate *priv)
{
  Window w = GDK_ROOT_WINDOW();

  XGrabKey(GDK_DISPLAY(), X_KEYCODE_UP, AnyModifier, w, True, GrabModeAsync,
           GrabModeAsync);
  XGrabKey(GDK_DISPLAY(), X_KEYCODE_DOWN, AnyModifier, w, True, GrabModeAsync,
           GrabModeAsync);
  priv->keys_are_grabbed = TRUE;
}

static void
ungrab_keys(SoundsStatusMenuItemPrivate *priv)
{
  XUngrabKey(GDK_DISPLAY(), X_KEYCODE_UP, AnyModifier, GDK_ROOT_WINDOW());
  XUngrabKey(GDK_DISPLAY(), X_KEYCODE_DOWN, AnyModifier, GDK_ROOT_WINDOW());
  priv->keys_are_grabbed = FALSE;
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

  if ((XGetWindowProperty(GDK_DISPLAY(), GDK_ROOT_WINDOW(), atom, 0,
                          sizeof(Window), False, XA_WINDOW, &type, &format,
                          &n_items, &bytes_after,
                          (unsigned char **)&val) == Success) &&
      (type == XA_WINDOW) && (format == 32) && (n_items == 1) && val)
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

    /* Presence is treated as the flag, deliberately.
     *
     * The canonical setter -- hildon_gtk_window_set_clear_window_flag()
     * in libhildon -- writes XA_INTEGER, format 32, one item, value 1,
     * and clears DND by DELETING the property.  So the only things a
     * conforming client can produce are "absent" and "INTEGER/32/1 = 1",
     * and a presence test answers both correctly.
     *
     * The desktop's own reader -- hd_comp_mgr_check_do_not_disturb_flag()
     * in hildon-desktop -- is stricter: it validates type, format and
     * item count and then requires the value to be exactly 1.  We
     * deliberately do not follow it.  This is a do-not-disturb flag, so
     * the conservative failure mode is the one that honours it: a client
     * that set the atom in some unusual way still does not want to be
     * disturbed, and suppressing a volume banner is harmless while
     * showing one against a user's explicit request is not.
     *
     * Worth recording: the XA_INTEGER type filter does NOT guard this
     * call.  Measured -- querying a CARDINAL or STRING property with
     * type=XA_INTEGER still returns Success with a non-NULL pointer and
     * nitems == 0.  So a wrong-typed property does reach the "is set"
     * branch here.  That is the outcome we want, but it is not the same
     * thing as having validated a type, and anyone tempted to treat the
     * filter as validation should know.
     */
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
                       &type, &format, &n_items, &bytes_after,
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

  if (*steps >= vol)
    return 0.0;

  if (vol >= steps[steps_size - 1])
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

  /* No ladder yet means no mapping to draw from. The control is
   * insensitive in this state; leave it where it is. */
  if (!have_ladder(priv))
    return;

  if (priv->call_active)
  {
    val = pa_vol_to_slider(priv->volume, priv->incall_volume_steps,
                           priv->incall_volume_num_steps);
  }
  else
  {
    val = pa_vol_to_slider(priv->volume, priv->normal_volume_steps,
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
          if (priv->keys_are_grabbed &&
          !priv->volume_changed &&
          ((hw_keycode == HW_KEYCODE_UP) || (hw_keycode == HW_KEYCODE_DOWN)) &&
          value)
      {
        priv->mm_key =
          (hw_keycode == HW_KEYCODE_UP ? X_KEYCODE_UP : X_KEYCODE_DOWN);
        read_sink_volume(menu_item);
      }
    }
    else if (error.message)
      g_warning("VOLUME: %s", error.message);
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
      g_warning("VOLUME: %s", error.message);
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

  if (priv->reconnect_id)
  {
    g_source_remove(priv->reconnect_id);
    priv->reconnect_id = 0;
  }

  teardown_connection(menu_item);

  if (priv->pw_bridge)
  {
    g_source_destroy(priv->pw_bridge);
    g_source_unref(priv->pw_bridge);
    priv->pw_bridge = NULL;
  }

  if (priv->pw_channel)
  {
    g_io_channel_unref(priv->pw_channel);
    priv->pw_channel = NULL;
  }

  if (priv->pw_loop)
  {
    pw_main_loop_destroy(priv->pw_loop);
    priv->pw_loop = NULL;
    pw_deinit();
  }

  if (priv->call_streams)
  {
    g_hash_table_destroy(priv->call_streams);
    priv->call_streams = NULL;
  }

  if (priv->sink_ids)
  {
    g_hash_table_destroy(priv->sink_ids);
    priv->sink_ids = NULL;
  }

  g_signal_handler_disconnect(gdk_screen_get_default(), priv->size_changed_id);

  if (priv->dbus)
  {
    dbus_connection_remove_filter(dbus_g_connection_get_connection(priv->dbus),
                                  dbus_filter, menu_item);
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

  if (g_key_file_load_from_file(key_file, CONFIGURATION_FILE, G_KEY_FILE_NONE,
                                NULL))
  {
    priv->normal_sink_name =
      g_key_file_get_string(key_file, "normal", "sink_name", &error);

    priv->normal_sink_name_provided = error ? FALSE : TRUE;

    if (error)
    {
      g_error_free(error);
      error = NULL;
    }

    priv->normal_sink_property =
      g_key_file_get_string(key_file, "normal", "sink_property", &error);

    /* A missing key means the convention, not "no ladder". These are
     * the keys the audio stack publishes the ladders under, so the
     * applet reads them even when sinks.ini says nothing -- otherwise a
     * bare or stale config file takes the volume control down over
     * what is only an absent override. */
    if (!priv->normal_sink_property || !*priv->normal_sink_property)
    {
      g_free(priv->normal_sink_property);
      priv->normal_sink_property = g_strdup(TUNING_KEY);
    }

    if (error)
      g_error_free(error);

    priv->incall_sink_property =
      g_key_file_get_string(key_file, "incall", "sink_property", &error);

    if (!priv->incall_sink_property || !*priv->incall_sink_property)
    {
      g_free(priv->incall_sink_property);
      priv->incall_sink_property = g_strdup(TUNING_INCALL_KEY);
    }

    if (error)
      g_error_free(error);

    priv->swap_on_rotate =
      g_key_file_get_boolean(key_file, "behavior", "swap_on_rotate", NULL);
    priv->native_landscape =
      g_key_file_get_boolean(key_file, "behavior", "native_is_landscape", NULL);
  }

  g_key_file_free(key_file);
}

/* Whether the table for the mode we are in can be mapped against.
 *
 * The ladder comes from the sink node, so between startup and the first
 * sink info -- and for a sink that publishes nothing -- there is none.
 * There is nothing to map a slider position to in that state, and a
 * write made from it would land on a level derived from nothing. The
 * slider is made insensitive and the volume keys are ignored instead of
 * substituting a curve.
 *
 * num_steps > 1 because a single entry cannot express a range. */
static gboolean
have_ladder(SoundsStatusMenuItemPrivate *priv)
{
  gint         n = priv->call_active ? priv->incall_volume_num_steps
                                    : priv->normal_volume_num_steps;
  gint      *tbl = priv->call_active ? priv->incall_volume_steps
                                    : priv->normal_volume_steps;

  return tbl != NULL && n > 1;
}

/* Reflect the ladder into the control. Called wherever the ladder is
 * installed or lost, so the widget never offers a gesture the applet
 * would have to discard. */
static void
sync_slider_sensitivity(SoundsStatusMenuItem *menu_item)
{
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);
  gboolean usable = have_ladder(priv);

  if (priv->hscale && gtk_widget_is_sensitive(priv->hscale) != usable)
    gtk_widget_set_sensitive(priv->hscale, usable);

  /* A level may have been parked waiting for exactly this. */
  if (usable && priv->deferred_state_sync)
  {
    priv->deferred_state_sync = FALSE;
    sink_state_changed(menu_item, "usable");
  }
}

static void
screen_size_changed_cb(GdkScreen *screen, gpointer user_data)
{
  SoundsStatusMenuItem *menu_item = SOUND_STATUS_MENU_ITEM(user_data);

  SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item)->portrait =
    gdk_screen_get_height(screen) > gdk_screen_get_width(screen);
}

/* Stream roles that mean "this stream is a call".
 *
 * libpulse documents media.role as one of "video", "music", "game",
 * "event", "phone", "animation", "production", "a11y", "test"
 * (proplist.h), so "phone" is the standard marker on the Pulse side and
 * is what Fremantle used.
 *
 * The PipeWire / XDG portal world uses a capitalised convention with
 * "Communication" for interactive voice, and wireplumber stores the role
 * verbatim -- formKey() in scripts/node/state-stream.lua performs no
 * normalisation -- so both spellings occur in practice in
 * ~/.local/state/wireplumber/stream-properties.  Match case-insensitively
 * against both so a call is not missed depending on who set the role.
 */
static const char *const call_media_roles[] = {
  "phone",
  "communication",
  NULL
};

static gboolean
is_call_media_role(const char *role)
{
  const char *const *r;

  if (!role)
    return FALSE;

  for (r = call_media_roles; *r; r++)
    if (g_ascii_strcasecmp(role, *r) == 0)
      return TRUE;

  return FALSE;
}

/* Call streams
 *
 * The in-call state comes from the stream list, not from an edge signal
 * out of MCE: the list is queryable at any moment, so an applet that
 * starts up during a call sees the truth instead of having missed a
 * transition it was not listening for.
 *
 * Whether a circuit-switched call exposes a host stream at all is a
 * property of the device, not something this plugin can assume.  On
 * mapphone the modem drives the codec directly through the UCM Voice
 * Call verb and there is nothing for the CPU to touch; on N900 the stream
 * is reachable; elsewhere we simply do not know.  The asymmetry costs
 * nothing here: a call stream that exists gets the in-call ladder, one
 * that does not leaves the sink ladder in charge, and both are the same
 * code path.
 */
struct call_stream
{
  SoundsStatusMenuItem *menu_item;
  uint32_t id;
  struct pw_proxy *proxy;
  struct spa_hook hook;
  gint volume;
  uint32_t channels;
  gboolean have_volume;
};

static void
call_stream_free(gpointer data)
{
  struct call_stream *cs = data;

  if (cs && cs->proxy)
    pw_proxy_destroy(cs->proxy);

  g_free(cs);
}

/* The stream whose level feeds the slider.  Prefer one that has actually
 * reported a volume, so a second call stream that is still being set up
 * cannot stall the display. */
static struct call_stream *
first_call_stream(SoundsStatusMenuItemPrivate *priv)
{
  GHashTableIter iter;
  gpointer value;
  struct call_stream *first = NULL;

  if (!priv->call_streams)
    return NULL;

  g_hash_table_iter_init(&iter, priv->call_streams);
  while (g_hash_table_iter_next(&iter, NULL, &value))
  {
    struct call_stream *cs = value;

    if (!first)
      first = cs;

    if (cs->have_volume)
      return cs;
  }

  return first;
}

static void
call_state_changed(SoundsStatusMenuItem *menu_item)
{
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);
  gboolean now_active;
  gboolean changed;
  struct call_stream *cs;

  now_active = priv->call_streams &&
                 g_hash_table_size(priv->call_streams) > 0;
  changed = priv->call_active != now_active;
  priv->call_active = now_active;

  if (!now_active)
  {
    if (changed)
    {
      g_debug("VOLUME: call state is now idle");
      /* Call over.  Put the slider back on the media target that was left
       * alone for the duration; if nothing ever published one, read the
       * sink so the seed path can adopt the route level. */
      if (priv->have_target)
      {
        priv->volume = priv->target;
        update_slider(menu_item);
      }
      else
        read_sink_volume(menu_item);
    }
    return;
  }

  if (changed)
    g_debug("VOLUME: call state is now active");

  cs = first_call_stream(priv);

  /* Active but nothing has reported a level yet: leave the display alone
   * rather than draw a number we do not have. */
  if (!cs || !cs->have_volume)
    return;

  if (changed || priv->volume != cs->volume)
  {
    /* The slider rides the call stream rather than the sink.  Leaving the
     * sink alone is the whole point: media keeps whatever level the user
     * left there while the call gets its own. */
    priv->volume = cs->volume;
    update_slider(menu_item);
  }
}

static void
call_node_info(void *data, const struct pw_node_info *info)
{
  struct call_stream *cs = data;

  /* A playback stream feeds the sink through its output ports, so that is
   * the channel count the write has to match. */
  if (info->n_output_ports > 0 && info->n_output_ports <= SPA_AUDIO_MAX_CHANNELS)
    cs->channels = info->n_output_ports;
}

static void
call_node_param(void *data, int seq, uint32_t id, uint32_t index,
                 uint32_t next, const struct spa_pod *param)
{
  struct call_stream *cs = data;

  (void) seq;
  (void) index;
  (void) next;

  if (id != SPA_PARAM_Props)
    return;

  if (read_pod_volume(param, &cs->volume))
    cs->have_volume = TRUE;

  call_state_changed(cs->menu_item);
}

static const struct pw_node_events call_node_events = {
  PW_VERSION_NODE_EVENTS,
  .info = call_node_info,
  .param = call_node_param,
};

static void
track_call_stream(SoundsStatusMenuItem *menu_item, uint32_t id)
{
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);
  struct call_stream *cs;
  uint32_t props[] = { SPA_PARAM_Props };

  if (!priv->call_streams ||
      g_hash_table_contains(priv->call_streams, GUINT_TO_POINTER(id)))
    return;

  cs = g_new0(struct call_stream, 1);
  cs->menu_item = menu_item;
  cs->id = id;
  cs->proxy = pw_registry_bind(priv->pw_registry, id, PW_TYPE_INTERFACE_Node,
                                PW_VERSION_NODE, 0);
  if (!cs->proxy)
  {
    g_warning("VOLUME: failed to bind call stream %u", id);
    g_free(cs);
    return;
  }

  pw_proxy_add_object_listener(cs->proxy, &cs->hook, &call_node_events, cs);

  /* Subscribe before asking for the current value, so a change landing
   * between the two cannot be missed. */
  pw_node_subscribe_params((struct pw_node *) cs->proxy, props, 1);
  pw_node_enum_params((struct pw_node *) cs->proxy, 0, SPA_PARAM_Props,
                      0, UINT32_MAX, NULL);

  g_hash_table_insert(priv->call_streams, GUINT_TO_POINTER(id), cs);

  call_state_changed(menu_item);
}

static void
untrack_call_stream(SoundsStatusMenuItem *menu_item, uint32_t id)
{
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);

  if (priv->call_streams &&
      g_hash_table_remove(priv->call_streams, GUINT_TO_POINTER(id)))
    call_state_changed(menu_item);
}

/* The default sink is a metadata entry rather than a field of a server
 * record: "default.audio.sink" on the core object, whose value is a JSON
 * string carrying the node name. */
static gboolean
default_sink_name_from_json(const char *json, gchar **out)
{
  struct spa_json doc;
  struct spa_json obj;
  char key[64];
  char val[512];

  if (!json)
    return FALSE;

  spa_json_init(&doc, json, strlen(json));

  if (spa_json_enter_object(&doc, &obj) <= 0)
    return FALSE;

  /* Keys and values alternate, so every value we do not want still has to
   * be consumed to reach the next key. */
  while (spa_json_get_string(&obj, key, sizeof(key)) > 0)
  {
    if (g_strcmp0(key, "name") == 0)
    {
      if (spa_json_get_string(&obj, val, sizeof(val)) <= 0)
        return FALSE;

      g_free(*out);
      *out = g_strdup(val);
      return TRUE;
    }

    if (spa_json_get_string(&obj, val, sizeof(val)) <= 0)
      return FALSE;
  }

  return FALSE;
}

/* Take a level for the sink we are tracking and put it to work.
 *
 * Split out because it has two callers arriving from different
 * directions: the metadata callback when the key changes, and the sink
 * bind when a level that was already there turns out to be aimed at us. */
static int
apply_target(SoundsStatusMenuItem *menu_item, const gchar *value)
{
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);
  gchar *end = NULL;
  gint64 n;

  if (!value)
  {
    priv->have_target = FALSE;
    return 0;
  }

  n = g_ascii_strtoll(value, &end, 10);
  if (!end || end == value || *end != '\0')
  {
    g_warning("VOLUME: %s on %s is not a number: '%s'", TARGET_KEY,
              priv->normal_sink_name ? priv->normal_sink_name : "(null)",
              value);
    return -EINVAL;
  }

  if (priv->have_target && priv->target == (gint) n)
    return 0;        /* the same level arriving again; nothing to redo */

  g_debug("VOLUME: target for %s is now %lld",
          priv->normal_sink_name ? priv->normal_sink_name : "(null)",
          (long long) n);

  priv->target = (gint) n;
  priv->have_target = TRUE;

  /* During a call the slider belongs to the stream; the target is still
   * recorded so the end of the call can put it back. */
  if (!priv->call_active)
    sink_state_changed(menu_item, "target");

  return 0;
}

static int
metadata_property(void *data, uint32_t subject, const char *key,
                   const char *type, const char *value)
{
  SoundsStatusMenuItem *menu_item = data;
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);

  (void) type;

  /* The target is a per-output value and the applet is the only writer of
   * it in metadata: this is the outbound half of the channel, from the
   * slider to the module. The level comes back the other way as a
   * property of the sink node, read in sink_info_cb(), where it arrives
   * together with the ladder that gives it meaning. So the echo of our
   * own write is not something to act on. */
  if (g_strcmp0(key, TARGET_KEY) == 0)
    return 0;

  if (subject != PW_ID_CORE || g_strcmp0(key, "default.audio.sink") != 0)
    return 0;

  if (!default_sink_name_from_json(value, &priv->default_sink_name))
  {
    g_warning("VOLUME: cannot read a sink name from '%s'", value ? value : "");
    return -EINVAL;
  }

  g_debug("VOLUME: default sink is now %s", priv->default_sink_name);

  follow_default_sink(menu_item);

  return 0;
}

static const struct pw_metadata_events metadata_events = {
  PW_VERSION_METADATA_EVENTS,
  .property = metadata_property,
};

static void
follow_default_sink(SoundsStatusMenuItem *menu_item)
{
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);

  /* A sink named in sinks.ini wins and is never replaced by the default. */
  if (priv->normal_sink_name_provided)
    return;

  if (!priv->default_sink_name)
    return;

  /* Only skip when we are actually still bound.  Comparing names alone is
   * not enough: after the bound sink disappears -- a profile switch, or a
   * node recreated under the same name with a new id -- the names still
   * match while sink_proxy is gone, and returning here left the applet
   * with no proxy at all until some unrelated event happened to call
   * resolve_sink().  The add path normally covers it, but that depends on
   * the new node being announced after the old one is removed; an
   * overlapping swap slipped through. */
  if (priv->sink_proxy &&
      priv->normal_sink_name &&
      g_str_equal(priv->normal_sink_name, priv->default_sink_name))
    return;

  g_free(priv->normal_sink_name);
  priv->normal_sink_name = g_strdup(priv->default_sink_name);

  resolve_sink(menu_item);
}

static void
registry_global(void *data, uint32_t id, uint32_t permissions,
                 const char *type, uint32_t version,
                 const struct spa_dict *props)
{
  SoundsStatusMenuItem *menu_item = data;
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);
  const char *media_class;

  (void) permissions;
  (void) version;

  if (g_strcmp0(type, PW_TYPE_INTERFACE_Metadata) == 0)
  {
    if (priv->pw_metadata)
      return;

    if (g_strcmp0(props ? spa_dict_lookup(props, "metadata.name") : NULL,
                   "default") != 0)
      return;

    priv->pw_metadata = (struct pw_metadata *) pw_registry_bind(
                          priv->pw_registry, id, PW_TYPE_INTERFACE_Metadata,
                          PW_VERSION_METADATA, 0);
    if (!priv->pw_metadata)
    {
      g_warning("VOLUME: failed to bind the default metadata object");
      return;
    }

    pw_metadata_add_listener(priv->pw_metadata, &priv->pw_metadata_hook,
                             &metadata_events, menu_item);
    return;
  }

  if (g_strcmp0(type, PW_TYPE_INTERFACE_Node) != 0)
    return;

  media_class = props ? spa_dict_lookup(props, PW_KEY_MEDIA_CLASS) : NULL;
  if (!media_class)
    return;

  if (g_strcmp0(media_class, "Audio/Sink") == 0)
  {
    const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);

    if (!name)
      return;

    /* Remember every sink we are told about.  The registry announces a
     * global once and there is no way to ask for the list again short of
     * tearing the registry down, so the names have to be kept on the
     * client side for a later resolve_sink(). */
    if (priv->sink_ids)
      g_hash_table_replace(priv->sink_ids, g_strdup(name),
                           GUINT_TO_POINTER(id));

    if (priv->normal_sink_name && g_str_equal(name, priv->normal_sink_name))
      bind_sink(menu_item, id);
    return;
  }

  if (g_strcmp0(media_class, "Stream/Output/Audio") == 0)
  {
    if (is_call_media_role(spa_dict_lookup(props, PW_KEY_MEDIA_ROLE)))
      track_call_stream(menu_item, id);
    return;
  }
}

static void
forget_sink_id(SoundsStatusMenuItemPrivate *priv, uint32_t id)
{
  GHashTableIter iter;
  gpointer key;
  gpointer value;

  if (!priv->sink_ids)
    return;

  g_hash_table_iter_init(&iter, priv->sink_ids);
  while (g_hash_table_iter_next(&iter, &key, &value))
  {
    if ((uint32_t) GPOINTER_TO_UINT(value) == id)
    {
      g_hash_table_iter_remove(&iter);
      return;
    }
  }
}

static void
registry_global_remove(void *data, uint32_t id)
{
  SoundsStatusMenuItem *menu_item = data;
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);

  if (priv->sink_id != SPA_ID_INVALID && priv->sink_id == id)
  {
    /* The sink is gone, usually a UCM profile switch, which moves the
     * default along with it.  Drop what was cached and re-follow. */
    clear_sink(menu_item);
    forget_sink_id(priv, id);
    follow_default_sink(menu_item);
    return;
  }

  forget_sink_id(priv, id);
  untrack_call_stream(menu_item, id);
}

static const struct pw_registry_events registry_events = {
  PW_VERSION_REGISTRY_EVENTS,
  .global = registry_global,
  .global_remove = registry_global_remove,
};

/* The done event closes the initial registry enumeration.  Until it
 * fires we have not necessarily seen the sink global or the
 * default.audio.sink metadata entry, so choosing a sink any earlier
 * would race the daemon. */
static void
core_done(void *data, uint32_t id, int seq)
{
  SoundsStatusMenuItem *menu_item = data;
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);

  (void) id;

  if (seq != priv->sync_seq)
    return;

  g_debug("VOLUME: initial PipeWire enumeration complete");

  if (priv->normal_sink_name_provided)
  {
    resolve_sink(menu_item);

    if (!priv->sink_proxy)
    {
      g_warning("VOLUME: sink '%s' is not present, following the default",
                priv->normal_sink_name ? priv->normal_sink_name : "");
      priv->normal_sink_name_provided = FALSE;
    }
  }

  if (!priv->normal_sink_name_provided)
    follow_default_sink(menu_item);

  /* Call streams are already tracked by the enumeration pass above; this
   * just lands the slider on the right ladder before the first draw. */
  call_state_changed(menu_item);
}

static void
core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
  SoundsStatusMenuItem *menu_item = data;

  (void) id;
  (void) seq;

  g_warning("VOLUME: PipeWire core error %d (%s)", res, message ? message : "");

  schedule_reconnect(menu_item);
}

static const struct pw_core_events core_events = {
  PW_VERSION_CORE_EVENTS,
  .done = core_done,
  .error = core_error,
};

/* Drain PipeWire whenever its epoll fd wakes.  pw_loop_get_fd() returns
 * the loop's real poll fd, not a wake-only pipe: IO sources, timerfds and
 * the cross-thread control eventfd are all registered into it, and an
 * epoll fd is itself pollable, so one GLib watch drives the whole
 * PipeWire client on the GTK thread.  Returning TRUE keeps it armed;
 * returning FALSE would leave the plugin deaf. */
static gboolean
pw_bridge_cb(GIOChannel *chan, GIOCondition cond, gpointer data)
{
  SoundsStatusMenuItem *menu_item = data;
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);

  (void) chan;
  (void) cond;

  if (!priv->pw_loop)
    return FALSE;

  if (pw_loop_iterate(pw_main_loop_get_loop(priv->pw_loop), false) < 0)
  {
    g_warning("VOLUME: PipeWire loop error, reconnecting");
    schedule_reconnect(menu_item);
  }

  return TRUE;
}

static void
teardown_connection(SoundsStatusMenuItem *menu_item)
{
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);

  if (priv->call_streams)
    g_hash_table_remove_all(priv->call_streams);

  clear_sink(menu_item);

  if (priv->pw_metadata)
  {
    spa_hook_remove(&priv->pw_metadata_hook);
    pw_proxy_destroy((struct pw_proxy *) priv->pw_metadata);
    priv->pw_metadata = NULL;
  }

  if (priv->pw_registry)
  {
    spa_hook_remove(&priv->pw_registry_hook);
    pw_proxy_destroy((struct pw_proxy *) priv->pw_registry);
    priv->pw_registry = NULL;
  }

  if (priv->pw_core)
  {
    spa_hook_remove(&priv->pw_core_hook);
    pw_core_disconnect(priv->pw_core);
    priv->pw_core = NULL;
  }

  if (priv->pw_context)
  {
    pw_context_destroy(priv->pw_context);
    priv->pw_context = NULL;
  }
}

static gboolean
reconnect_timer(gpointer data)
{
  SoundsStatusMenuItem *menu_item = data;
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);

  priv->reconnect_id = 0;
  reconnect(menu_item);

  return G_SOURCE_REMOVE;
}

static void
schedule_reconnect(SoundsStatusMenuItem *menu_item)
{
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);

  if (priv->reconnect_id)
    return;

  priv->reconnect_id = g_timeout_add_seconds(RECONNECT_DELAY_SECONDS,
                                             reconnect_timer, menu_item);
}

static void
reconnect(SoundsStatusMenuItem *menu_item)
{
  SoundsStatusMenuItemPrivate *priv;

  g_assert(menu_item);

  priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);
  g_assert(priv);

  teardown_connection(menu_item);

  priv->pw_context = pw_context_new(pw_main_loop_get_loop(priv->pw_loop),
                                    NULL, 0);
  if (!priv->pw_context)
  {
    g_warning("VOLUME: cannot create a PipeWire context, retrying");
    schedule_reconnect(menu_item);
    return;
  }

  priv->pw_core = pw_context_connect(priv->pw_context, NULL, 0);
  if (!priv->pw_core)
  {
    g_warning("VOLUME: cannot connect to PipeWire, retrying");
    pw_context_destroy(priv->pw_context);
    priv->pw_context = NULL;
    schedule_reconnect(menu_item);
    return;
  }

  pw_core_add_listener(priv->pw_core, &priv->pw_core_hook, &core_events,
                       menu_item);

  priv->pw_registry = pw_core_get_registry(priv->pw_core, PW_VERSION_REGISTRY,
                                          0);
  if (!priv->pw_registry)
  {
    g_warning("VOLUME: cannot get the PipeWire registry, retrying");
    schedule_reconnect(menu_item);
    return;
  }

  pw_registry_add_listener(priv->pw_registry, &priv->pw_registry_hook,
                           &registry_events, menu_item);

  /* Everything announced during the enumeration arrives before the done
   * event for this sync, which is what makes it the load-complete mark. */
  priv->sync_seq = pw_core_sync(priv->pw_core, PW_ID_CORE, 0);
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

  if (num_steps < 2)
    return 0.0;

  pa_vol = slider_to_pa_vol(volume, steps, num_steps) - 1;

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

/* Drop a table so the mode it belongs to has nothing to map against.
 *
 * parse_tuning_property() deliberately keeps the current table when the
 * property is absent, which is right for a re-read of the same sink but
 * wrong across a device change: the new sink would be mapped with the
 * old device's curve. Absence is made explicit here so the control goes
 * insensitive instead of going quietly wrong. */
static void
clear_tuning(gint *num_steps_out, gint **steps_out, GQuark *quark)
{
  g_free(*steps_out);
  *steps_out = NULL;
  *num_steps_out = 0;
  *quark = 0;
}

static gboolean
parse_tuning_property(const gchar *property, gint *num_steps_out,
                      gint **steps_out, GQuark *quark)
{
  gchar **steps_array;
  gint i, l;
  gint num_steps = 1;
  gint *steps;
  GQuark q;

  /* No property at all: keep whatever table we already have. */
  if (!property)
    return FALSE;

  q = g_quark_from_string(property);

  if (q == *quark)
    return FALSE;

  *quark = q;

  /* Format: a comma separated list of gain values in centibel, ascending,
   * for example "-4000,-3800,...,-200,0". Index 0 of the table built here
   * is always silence; the listed values are the real volume steps.
   *
   * This is deliberately not the Nokia "alsa_value:dB" form. The leading
   * ALSA control value was only ever consumed by module-alsa-sink-volume,
   * which does not exist on Leste; with UCM the audio stack programs the
   * hardware itself, so carrying that column would only invite people to
   * invent plausible numbers for a field nothing reads. */
  steps_array = g_strsplit(property, ",", -1);
  l = g_strv_length(steps_array);
  steps = g_new(gint, l + 1);
  steps[0] = 0;

  for (i = 0; i < l; i++)
  {
    const gchar *entry = steps_array[i];
    gchar *end = NULL;
    long centibel;
    gint linear;

    if (!entry || !*entry)
      continue;

    centibel = strtol(entry, &end, 10);

    if (end == entry)
    {
      g_warning("VOLUME: ignoring tuning step '%s', not a number", entry);
      continue;
    }

    /* strtol() stops at the first character it cannot read, so without this
     * a leftover "alsa_value:dB" table would parse as 1 cB, 31 cB, 118 cB -
     * a nearly-silent table instead of a rejected one. Nothing may follow
     * the number but whitespace. */
    while (*end == ' ' || *end == '\t')
      end++;

    if (*end != '\0')
    {
      g_warning("VOLUME: ignoring tuning step '%s', trailing garbage after "
                "the value (expected a plain centibel number)", entry);
      continue;
    }

    linear = tuning_cb_to_volume((gint)centibel);

    /* Both mapping functions interpolate between neighbouring entries and
     * divide by their difference, so an entry that is not strictly above
     * the previous one would divide by zero or map backwards. */
    if (num_steps > 1 && linear <= steps[num_steps - 1])
    {
      g_warning("VOLUME: tuning step %ld cB does not exceed the previous "
                "one, ignoring it (the table must be ascending)", centibel);
      continue;
    }

    steps[num_steps++] = linear;
  }

  g_strfreev(steps_array);

  /* A table with a single entry is useless and would trip the g_assert()
   * in slider_to_pa_vol() / pa_vol_to_slider(). Keep the previous table. */
  if (num_steps < 2)
  {
    g_free(steps);
    return FALSE;
  }

  *num_steps_out = num_steps;
  g_free(*steps_out);
  *steps_out = (gint *)g_realloc(steps, num_steps * sizeof(steps[0]));

  return TRUE;
}

/* The tracked sink.
 *
 * Two halves arrive on two different channels: the tuning properties and
 * the channel count come on node.info, the level comes on the
 * SPA_PARAM_Props param.  The slider needs both, so each half records its
 * arrival and only the second one runs the shared tail. */
static void
sink_info_cb(void *data, const struct pw_node_info *info)
{
  SoundsStatusMenuItem *menu_item = data;
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);
  const struct spa_dict *props;
  const char *prop_normal;
  const char *prop_incall;
  const char *level;
  const char *name;

  props = info->props;
  name = props ? spa_dict_lookup(props, PW_KEY_NODE_NAME) : NULL;

  if (!name || !priv->normal_sink_name ||
      !g_str_equal(name, priv->normal_sink_name))
    return;

  /* The sink's input ports are where streams land, so their count is the
   * layout width.  This has to be re-read on every info rather than latched
   * on first sight: after the default sink changes to a device with a
   * different layout, keeping the old count makes the write invalid and it
   * gets rejected without any visible symptom. */
  if (info->n_input_ports == 0 || info->n_input_ports > SPA_AUDIO_MAX_CHANNELS)
  {
    g_warning("VOLUME: sink %s reports %u input ports, not a usable channel "
              "count", name, info->n_input_ports);
    return;
  }

  priv->normal_channels = (guint8) info->n_input_ports;

  prop_normal = priv->normal_sink_property ?
                  spa_dict_lookup(props, priv->normal_sink_property) : NULL;

  if (prop_normal)
    priv->warned_normal_tuning = FALSE;
  else
  {
    /* Say it once per sink change. A missing wireplumber rule should be
     * visible in the log, and the control should stop rather than carry
     * on against a curve that merely looks plausible. */
    if (!priv->warned_normal_tuning)
    {
      priv->warned_normal_tuning = TRUE;
      g_warning("VOLUME: sink %s publishes no %s; the volume control is "
                "disabled until one appears",
                name,
                priv->normal_sink_property ?
                  priv->normal_sink_property : "tuning property");
    }
    clear_tuning(&priv->normal_volume_num_steps,
                 &priv->normal_volume_steps, &priv->quark_normal);
  }

  if (prop_normal)
    parse_tuning_property(prop_normal, &priv->normal_volume_num_steps,
                         &priv->normal_volume_steps, &priv->quark_normal);

  prop_incall = priv->incall_sink_property ?
                  spa_dict_lookup(props, priv->incall_sink_property) : NULL;

  if (prop_incall)
    priv->warned_incall_tuning = FALSE;
  else
  {
    if (!priv->warned_incall_tuning)
    {
      priv->warned_incall_tuning = TRUE;
      g_warning("VOLUME: sink %s publishes no %s; the in-call volume "
                "control is disabled until one appears",
                name,
                priv->incall_sink_property ?
                  priv->incall_sink_property : "in-call tuning property");
    }
    clear_tuning(&priv->incall_volume_num_steps,
                 &priv->incall_volume_steps, &priv->quark_incall);
  }

  if (prop_incall)
    parse_tuning_property(prop_incall, &priv->incall_volume_num_steps,
                         &priv->incall_volume_steps, &priv->quark_incall);

  priv->have_sink_props = TRUE;

  /* The level this output is being driven at. It is a property of the
   * node itself, so it arrives in the same info as the ladder -- the
   * value and the curve that gives it meaning are never separable. That
   * is what makes a device switch self-correcting: binding the new sink
   * brings its level with it, on a cold start as much as on a switch,
   * with nothing to have missed in between. */
  level = spa_dict_lookup(props, TARGET_KEY);
  if (level)
    apply_target(menu_item, level);

  /* The ladder has just arrived from the sink node. Bring the control up
   * and put it where the restored level says it belongs. */
  sync_slider_sensitivity(menu_item);

  if (priv->have_sink_volume)
    sink_state_changed(menu_item, "sink-volume");
}

static void
sink_param_cb(void *data, int seq, uint32_t id, uint32_t index,
               uint32_t next, const struct spa_pod *param)
{
  SoundsStatusMenuItem *menu_item = data;
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);

  (void) seq;
  (void) index;
  (void) next;

  if (id != SPA_PARAM_Props)
    return;



  priv->have_sink_volume = TRUE;

  /* No seeding here.  The node mirrors the route only while the sink is
   * running; suspended, it keeps whatever it last had.  Adopting from it
   * would overwrite the stored per-port level with a stale number, so the
   * policy -- which reads the route itself -- owns the initial target. */
  if (priv->have_sink_props)
    sink_state_changed(menu_item, "sink-props");
}

static const struct pw_node_events sink_node_events = {
  PW_VERSION_NODE_EVENTS,
  .info = sink_info_cb,
  .param = sink_param_cb,
};

static void
read_sink_volume(SoundsStatusMenuItem *menu_item)
{
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);

  if (!priv->sink_proxy)
    return;

  pw_node_enum_params((struct pw_node *) priv->sink_proxy, 0, SPA_PARAM_Props,
                      0, UINT32_MAX, NULL);
}

/* Publish what the user asked for and remember it as both the target and
 * the displayed level.  The echo through the metadata callback is what
 * confirms it; claiming it here is what stops the seed path below from
 * firing again on the next sink notification. */
static void
write_target(SoundsStatusMenuItem *menu_item, gint volume)
{
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);
  gchar text[16];

  if (!priv->pw_metadata)
  {
    g_warning("VOLUME: no metadata object, cannot set a volume target");
    return;
  }

  if (priv->sink_id == SPA_ID_INVALID)
  {
    g_warning("VOLUME: not tracking a sink, cannot set a volume target");
    return;
  }

  g_snprintf(text, sizeof(text), "%d", volume);

  if (pw_metadata_set_property(priv->pw_metadata, priv->sink_id,
                                TARGET_KEY, "Spa:Int", text) < 0)
  {
    g_warning("VOLUME: failed to set a volume target on %s",
              priv->normal_sink_name ? priv->normal_sink_name : "(null)");
    return;
  }

  priv->target = volume;
  priv->volume = volume;
  priv->have_target = TRUE;

  g_debug("VOLUME: target %d on %s (%u)", volume,
          priv->normal_sink_name ? priv->normal_sink_name : "(null)",
          priv->sink_id);
}

static void
bind_sink(SoundsStatusMenuItem *menu_item, uint32_t id)
{
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);
  uint32_t props[] = { SPA_PARAM_Props };

  if (priv->sink_proxy)
  {
    if (priv->sink_id == id)
      return;

    clear_sink(menu_item);
  }

  priv->sink_proxy = pw_registry_bind(priv->pw_registry, id,
                                      PW_TYPE_INTERFACE_Node, PW_VERSION_NODE,
                                      0);
  if (!priv->sink_proxy)
  {
    g_warning("VOLUME: failed to bind sink %u (%s)", id,
              priv->normal_sink_name ? priv->normal_sink_name : "");
    return;
  }

  priv->sink_id = id;

  pw_proxy_add_object_listener(priv->sink_proxy, &priv->sink_hook,
                               &sink_node_events, menu_item);

  /* Subscribe first, then read, so a change landing between the two
   * cannot be missed. */
  pw_node_subscribe_params((struct pw_node *) priv->sink_proxy, props, 1);
  read_sink_volume(menu_item);
}

static void
clear_sink(SoundsStatusMenuItem *menu_item)
{
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);

  if (priv->sink_proxy)
  {
    spa_hook_remove(&priv->sink_hook);
    pw_proxy_destroy(priv->sink_proxy);
    priv->sink_proxy = NULL;
  }

  priv->sink_id = SPA_ID_INVALID;
  priv->have_sink_props = FALSE;
  priv->have_sink_volume = FALSE;
  priv->have_target = FALSE;
  priv->target = 0;
  priv->normal_channels = 0;
}

static void
resolve_sink(SoundsStatusMenuItem *menu_item)
{
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);
  gpointer value;

  if (!priv->sink_ids || !priv->normal_sink_name)
    return;

  if (!g_hash_table_lookup_extended(priv->sink_ids, priv->normal_sink_name,
                                   NULL, &value))
  {
    g_debug("VOLUME: sink %s has not appeared yet", priv->normal_sink_name);
    return;
  }

  /* Skip only when this is the sink we are already on. Skipping whenever
   * a proxy merely exists means a move to a different sink that is still
   * present never happens: the old proxy keeps being followed, and the
   * new device's ladder and level are never read -- a slider left showing
   * one output while another is playing. */
  if (priv->sink_proxy &&
      priv->sink_id == (uint32_t) GPOINTER_TO_UINT(value))
    return;

  bind_sink(menu_item, (uint32_t) GPOINTER_TO_UINT(value));
}

/* Shared tail: pick the ladder for the current mode, sync the display, and
 * turn a waiting key press into a step. */
static void
sink_state_changed(SoundsStatusMenuItem *menu_item, const char *why)
{
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);
  gboolean volume_changed = FALSE;
  gint volume;
  gint steps_size;
  gint *steps;
  double current_vol;
  double new_vol;

  /* The source of truth for the slider depends on the mode.
   *
   * Out of a call it is the sink.  During one it is the call stream: the
   * sink says nothing about a level we deliberately keep off it, and
   * copying the sink level here would wipe the call volume the moment any
   * unrelated sink event arrived.
   *
   * Either way this has to happen before the mm_key bail-out below -- the
   * applet needs the current level just to draw the slider, and since the
   * role-based detector replaced the stream-restore read callback there is
   * no other producer for the media case. */
  if (priv->call_active)
    volume = priv->volume;
  else
  {
    /* Out of a call the slider reads the target, never the sink.  Reading
     * the sink would let the policy move the control, and the next
     * relative step is computed from whatever is showing -- so a policy
     * lift for an alarm would silently rebase the user's ladder. */
    if (!priv->have_target)
      { g_debug("VOLUME T: gate blocked, no target"); return; }

    volume = priv->target;
    priv->volume = volume;
  }

  if (priv->call_active)
  {
    steps = priv->incall_volume_steps;
    steps_size = priv->incall_volume_num_steps;
  }
  else
  {
    steps = priv->normal_volume_steps;
    steps_size = priv->normal_volume_num_steps;
  }

  /* Nothing to map the reported level onto, and no key step to serve.
   * The keys are ignored rather than guessed at: a step computed against
   * a missing table would move the hardware to a level nobody chose. A
   * level we already hold is parked so the ladder can replay it, which
   * is the common case at startup -- the target and the ladder arrive on
   * different channels. */
  if (steps == NULL || steps_size < 2)
  {
    priv->deferred_state_sync = TRUE;
    return;
  }

  /* Not a volume change: this runs for every sink event, the target
   * arriving back from the metadata, the node's volume mirror, the node's
   * properties, a change of usability, and it prints the target we hold,
   * not a value read from the sink. One key press produces four of these
   * for one level, so only a change of the level shown is worth a line. */
  {
    static gint last_shown = -1;

    if (priv->volume != last_shown)
      {
        last_shown = priv->volume;
        g_debug("VOLUME: %s(%s): %s level shown is %i", __func__, why,
                priv->call_active ? "in-call" : "normal", priv->volume);
      }
  }

  current_vol = pa_vol_to_slider(volume, steps, steps_size);

  /* Nothing beyond keeping the display in sync unless a key press is waiting
   * on this read.
   *
   * X_KEYCODE_UP/X_KEYCODE_DOWN are XKeysymToKeycode() results and come back
   * 0 when the keymap has no XF86XK_AudioRaiseVolume/XF86XK_AudioLowerVolume.
   * With mm_key idle at 0 the comparisons further down would then evaluate
   * 0 == 0 and turn every sink notification into a volume-up step.  This
   * bail-out is the guard commit 585a6ab dropped when it moved the comparisons
   * from raw keycodes to X keysyms.
   */
  if (!priv->mm_key)
  {
    /* Only sync the widget when no key press owns it.
     *
     * This call is what lets a read racing the banner drag the slider back.
     * The original prop_sink_info_cb() never touched the slider outside the
     * keypress path -- it set priv->volume and went straight to the key
     * comparisons -- so an async read could not move the widget at all.
     * Populating the slider here was needed for a cold start, but it made
     * every read a writer to the same widget the banner reads at expose
     * time, and a sink change notification fires this read on every CHANGE,
     * including the ones PipeWire emits for state, latency and port changes
     * that have nothing to do with volume.  A read issued before our write
     * lands can therefore complete after we have already moved the slider
     * forward and pull it back to the pre-write value, which is the banner
     * showing a level one press behind.
     *
     * volume_changed is set for the window right after a write, which
     * covers press -> expose.  Cold start still populates, because then
     * nothing has been written yet. */
    if (!priv->volume_changed)
      update_slider(menu_item);
    goto out;
  }

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

    if (!priv->parent_window_mapped &&
        !hildon_get_dnd(hildon_window_get_active_window()))
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

/* Push the current level to every tracked call stream.
 *
 * This is what makes media and VOIP independent.  Writing the sink moves
 * the call and the media together; writing the stream leaves the media
 * sink exactly where the user left it.  wireplumber's session restore is
 * keyed on media.role, so the call level also survives a stack restart
 * with nothing for this applet to persist.
 *
 * Every call stream gets the same value: "the call volume" is one number,
 * not one per app. */
static void
apply_call_volume(SoundsStatusMenuItem *menu_item)
{
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);
  GHashTableIter iter;
  gpointer value;

  if (!priv->call_streams)
    return;

  g_hash_table_iter_init(&iter, priv->call_streams);
  while (g_hash_table_iter_next(&iter, NULL, &value))
  {
    struct call_stream *cs = value;
    uint8_t buf[256];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));

    if (!cs->proxy || cs->channels == 0)
      continue;

    if (pw_node_set_param((struct pw_node *) cs->proxy, SPA_PARAM_Props,
                          0, build_volume_pod(&b, priv->volume,
                                               cs->channels)) < 0)
    {
      g_warning("VOLUME: failed to set call stream %u", cs->id);
      continue;
    }

    g_debug("VOLUME: pushed %d to call stream %u", priv->volume, cs->id);
  }
}

static void
set_volume(SoundsStatusMenuItem *menu_item, int volume)
{
  SoundsStatusMenuItemPrivate *priv;

  g_return_if_fail(menu_item);

  priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);

  /* Route by mode.  In a call the level belongs to the stream so that
   * media volume is not dragged along with it; out of a call nothing is
   * written to the sink at all -- the target goes to the policy and the
   * policy drives the route. */
  priv->volume = volume;

  if (priv->call_active)
    apply_call_volume(menu_item);
  else
    write_target(menu_item, volume);
}

static void
hscale_value_changed_cb(GtkRange *range, gpointer user_data)
{
  SoundsStatusMenuItem *menu_item = (SoundsStatusMenuItem *)user_data;
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);
  int pa_vol;

  priv->range_val = gtk_range_get_value(GTK_RANGE(range));

  /* Belt against a drag that slips in while the table is gone; the widget
   * is insensitive so this should not fire, but a write made from no
   * ladder would set the volume from nothing. */
  if (!have_ladder(priv))
    return;

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

  set_volume(menu_item, pa_vol);
  set_volume_icon(menu_item, gtk_range_get_value(GTK_RANGE(range)) * 100.0f);
}

static void
parent_window_map_cb(GtkWidget *widget, SoundsStatusMenuItem *menu_item)
{
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);

  grab_keys(priv);

  read_sink_volume(menu_item);

  priv->parent_window_mapped = TRUE;
  update_slider(menu_item);
}

static void
parent_window_unmap_cb(GtkWidget *widget, SoundsStatusMenuItem *menu_item)
{
  SoundsStatusMenuItemPrivate *priv = SOUND_STATUS_MENU_ITEM_PRIVATE(menu_item);
  gboolean ret;

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
  GtkWidget *hbox;
  DBusConnection *conn;
  GError *error = NULL;
  GdkScreen *screen;

  XSetErrorHandler(x_error_handler);

  priv->volume_changed = FALSE;
  priv->call_active = FALSE;
  priv->sink_id = SPA_ID_INVALID;
  priv->parent_signals_connected = FALSE;
  priv->parent_window_mapped = FALSE;
  priv->mm_key = 0;
  priv->icon = NULL;
  priv->normal_channels = 0;
  priv->swap_on_rotate = FALSE;
  priv->display_on = TRUE;
  priv->keys_are_grabbed = FALSE;
  priv->native_landscape = FALSE;

  get_sinks(priv);
  grab_keys(priv);

  gdk_window_set_events(GDK_ROOT_PARENT(),
                        gdk_window_get_events(GDK_ROOT_PARENT()) |
                        GDK_PROPERTY_CHANGE_MASK);

  gdk_window_add_filter(0, gdk_filter_func, menu_item);

  screen = gdk_screen_get_default();
  priv->portrait = gdk_screen_get_height(screen) > gdk_screen_get_width(screen);
  priv->size_changed_id = g_signal_connect(
      screen, "size-changed", G_CALLBACK(screen_size_changed_cb), menu_item);

  priv->call_streams = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                                             call_stream_free);
  priv->sink_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                         NULL);

  /* pw_main_loop_new() does not do this for us; without it the SPA plugin
   * directory is unset and the loop cannot load its support.system handle. */
  pw_init(NULL, NULL);

  priv->pw_loop = pw_main_loop_new(NULL);
  if (!priv->pw_loop)
  {
    g_warning("VOLUME: cannot create the PipeWire main loop; volume control "
              "will be unavailable");
  }
  else if (pw_loop_get_fd(pw_main_loop_get_loop(priv->pw_loop)) < 0)
  {
    g_warning("VOLUME: the PipeWire main loop has no poll fd; volume control "
              "will be unavailable");
    pw_main_loop_destroy(priv->pw_loop);
    priv->pw_loop = NULL;
  }
  else
  {
    /* Drive PipeWire off the GTK main loop.  pw_loop_get_fd() is the
     * loop's real epoll fd -- IO sources, timerfds and the cross-thread
     * control eventfd are all registered into it -- so watching it and
     * calling pw_loop_iterate() with block=false runs every PipeWire
     * callback on this thread alongside the X key grabs and the MCE
     * D-Bus filter.  The channel does not own the fd, so it must not
     * close it; the loop does. */
    priv->pw_channel = g_io_channel_unix_new(
                         pw_loop_get_fd(pw_main_loop_get_loop(priv->pw_loop)));
    g_io_channel_set_close_on_unref(priv->pw_channel, FALSE);

    priv->pw_bridge = g_io_create_watch(priv->pw_channel, G_IO_IN);
    g_source_set_callback(priv->pw_bridge, (GSourceFunc) pw_bridge_cb,
                           menu_item, NULL);
    g_source_attach(priv->pw_bridge, g_main_context_default());

    reconnect(menu_item);
  }

  priv->dbus = dbus_g_bus_get(DBUS_BUS_SYSTEM, &error);

  if (!priv->dbus)
  {
    g_warning("VOLUME: Failed to open connection to bus: %s", error->message);
    return;
  }

  conn = dbus_g_connection_get_connection(priv->dbus);

  dbus_bus_add_match(conn, DBUS_MCE_KEY_MATCH_RULE, NULL);
  dbus_bus_add_match(conn, DBUS_MCE_DISPLAY_MATCH_RULE, NULL);
  dbus_connection_add_filter(conn, dbus_filter, menu_item, NULL);

  hbox = gtk_hbox_new(FALSE, 0);

  priv->image = gtk_image_new();
  gtk_box_pack_start(GTK_BOX(hbox), priv->image, FALSE, FALSE, 8);

  priv->hscale = hildon_gtk_hscale_new();
  /* Nothing to map until a sink publishes a ladder. */
  gtk_widget_set_sensitive(priv->hscale, FALSE);
  gtk_box_pack_start(GTK_BOX(hbox), priv->hscale, TRUE, TRUE, 8);
  priv->hscale_value_changed_id =
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
