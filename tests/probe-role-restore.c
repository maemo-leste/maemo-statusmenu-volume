/*
 * Does a written sink-input-by-media-role:<role> entry actually get APPLIED
 * by pipewire-pulse to a newly-appearing stream carrying that role?
 *
 * Persistence alone is useless -- we need to know whether the restore drives
 * the volume of a real stream.
 *
 *   ./probe-role-restore            # writes the entry, then watches
 *   paplay --property=media.role=phone /tmp/test.wav     # in another shell
 *
 * Expected if role restore works: the phone stream comes up at VOL (12345).
 * If it comes up at some other value, role-based restore is NOT applied and
 * the per-stream design needs a different persistence mechanism.
 */

#include <pulse/pulseaudio.h>
#include <pulse/ext-stream-restore.h>
#include <pulse/glib-mainloop.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>

#define PHONE_VOL 12345u

static GMainLoop *loop;

static const char *tname(unsigned t)
{
    switch (t) {
    case PA_SUBSCRIPTION_EVENT_NEW:    return "NEW";
    case PA_SUBSCRIPTION_EVENT_CHANGE: return "CHANGE";
    case PA_SUBSCRIPTION_EVENT_REMOVE: return "REMOVE";
    default:                          return "?";
    }
}

static void si_info_cb(pa_context *c, const pa_sink_input_info *i, int eol, void *ud)
{
    (void)c; (void)ud;
    if (eol != 0 || !i)
        return;

    const char *role = pa_proplist_gets(i->proplist, PA_PROP_MEDIA_ROLE);
    const char *app  = pa_proplist_gets(i->proplist, PA_PROP_APPLICATION_NAME);
    const char *bin  = pa_proplist_gets(i->proplist, PA_PROP_APPLICATION_PROCESS_BINARY);
    const char *srid = pa_proplist_gets(i->proplist, "module-stream-restore.id");
    pa_volume_t avg  = pa_cvolume_avg(&i->volume);

    printf("  SINK_INPUT #%u  app=%s  binary=%s  role=%s  vol=%u (%u%%)  sr_id=%s\n",
           i->index, app ? app : "-", bin ? bin : "-",
           role ? role : "(none)",
           (unsigned)avg, (unsigned)(avg * 100 / PA_VOLUME_NORM),
           srid ? srid : "-");

    if (role && strcmp(role, "phone") == 0) {
        if (avg == PHONE_VOL)
            printf("      --> phone stream came up at the RESTORED volume %u: "
                  "role restore IS applied\n", PHONE_VOL);
        else
            printf("      --> phone stream came up at %u, NOT the written %u: "
                  "role restore is NOT applied\n", (unsigned)avg, PHONE_VOL);
    }
}

static void dump_sink_inputs(pa_context *c)
{
    pa_context_get_sink_input_info_list(c, si_info_cb, NULL);
}

static void sub_cb(pa_context *c, pa_subscription_event_type_t t, uint32_t idx, void *ud)
{
    unsigned facility = t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;

    if (facility != PA_SUBSCRIPTION_EVENT_SINK_INPUT)
        return;

    printf("[event] SINK_INPUT %s idx=%u\n", tname(t & PA_SUBSCRIPTION_EVENT_TYPE_MASK), idx);
    if ((t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) != PA_SUBSCRIPTION_EVENT_REMOVE)
        dump_sink_inputs(c);
    (void)ud;
}

static void sr_read_cb(pa_context *c, const pa_ext_stream_restore_info *info,
                      int eol, void *ud)
{
    (void)c; (void)ud;
    if (eol != 0 || !info)
        return;
    printf("  entry name='%s' vol=%u mute=%d device=%s\n",
           info->name, (unsigned)pa_cvolume_avg(&info->volume), info->mute,
           info->device ? info->device : "(null)");
}

static gboolean dump_now(gpointer c) { dump_sink_inputs((pa_context *)c); return FALSE; }

static gboolean quit_now(gpointer d) { g_main_loop_quit((GMainLoop *)d); return FALSE; }

static void after_subscribe_cb(pa_context *c, int success, void *ud)
{
    (void)ud;
    printf("--- subscribed to SINK_INPUT events (success=%d) ---\n", success);
    printf("    now run:  paplay --property=media.role=phone /tmp/test.wav\n\n");
    g_timeout_add(3000, dump_now, c);
    g_timeout_add(40000, quit_now, loop);
}

static void after_write_cb(pa_context *c, int success, void *ud)
{
    printf("--- wrote sink-input-by-media-role:phone = %u (success=%d) ---\n",
           PHONE_VOL, success);
    printf("--- read back ---\n");
    pa_ext_stream_restore_read(c, sr_read_cb, ud);
    pa_context_subscribe(c, PA_SUBSCRIPTION_MASK_SINK_INPUT, after_subscribe_cb, ud);
}

static void after_test_cb(pa_context *c, uint32_t version, void *ud)
{
    pa_ext_stream_restore_info info[2];

    printf("ext_stream_restore_test: version=%u\n", version);

    memset(&info[0], 0, sizeof(info[0]));
    pa_channel_map_init_stereo(&info[0].channel_map);
    pa_cvolume_set(&info[0].volume, 2, PHONE_VOL);
    info[0].name   = "sink-input-by-media-role:phone";
    info[0].device = NULL;
    info[0].mute   = 0;

    /* Control: does ANY restore key work?  If the app-name entry is applied
     * but the role entry is not, the gap is role-specific.  If neither is,
     * pipewire-pulse has no restore behaviour at all. */
    memset(&info[1], 0, sizeof(info[1]));
    pa_channel_map_init_stereo(&info[1].channel_map);
    pa_cvolume_set(&info[1].volume, 2, 22222);
    info[1].name   = "sink-input-by-application-name:paplay";
    info[1].device = NULL;
    info[1].mute   = 0;

    pa_ext_stream_restore_write(c, PA_UPDATE_REPLACE, info, 2, TRUE,
                               after_write_cb, ud);
}

static void state_cb(pa_context *c, void *ud)
{
    pa_subscription_mask_t m = PA_SUBSCRIPTION_MASK_SINK_INPUT;
    (void)m;
    if (pa_context_get_state(c) != PA_CONTEXT_READY)
        return;
    pa_context_set_subscribe_callback(c, sub_cb, ud);
    pa_ext_stream_restore_test(c, after_test_cb, ud);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    pa_glib_mainloop *ml = pa_glib_mainloop_new(NULL);
    pa_mainloop_api *api = pa_glib_mainloop_get_api(ml);
    pa_context *c = pa_context_new(api, "probe-role-restore");

    loop = g_main_loop_new(NULL, FALSE);

    if (pa_context_connect(c, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0) {
        fprintf(stderr, "connect failed: %s\n", pa_strerror(pa_context_errno(c)));
        return 1;
    }
    pa_context_set_state_callback(c, state_cb, c);

    g_main_loop_run(loop);

    pa_context_disconnect(c);
    pa_context_unref(c);
    pa_glib_mainloop_free(ml);
    return 0;
}
