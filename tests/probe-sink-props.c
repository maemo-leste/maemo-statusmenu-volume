/* Probe what libpulse actually reports about a sink's volume capabilities.
 *
 * The question this answers: can the applet build its slider table purely
 * from the pulse API, or does it need the audio stack to declare the
 * hardware step structure?
 *
 *   gcc -o /tmp/probe-sink-props probe-sink-props.c \
 *       $(pkg-config --cflags --libs glib-2.0) -lpulse -lpulse-mainloop-glib
 *   /tmp/probe-sink-props              # default sink
 *   /tmp/probe-sink-props alsa_output.platform-soundcard.HiFi__Speaker__sink
 *
 * Read-only: it only introspects, it never sets anything.
 */
#include <glib.h>
#include <pulse/pulseaudio.h>
#include <pulse/glib-mainloop.h>
#include <stdio.h>
#include <string.h>

static int done = 0;

static void
sink_cb(pa_context *c, const pa_sink_info *i, int eol, void *u)
{
    (void) c; (void) u;

    if (eol)
    {
        done = 1;
        return;
    }
    if (!i)
        return;

    printf("sink: %s\n", i->name);
    printf("  channels         = %u\n", i->channel_map.channels);
    printf("  volume.values[0] = %-7u (%.2f dB)\n",
           i->volume.values[0], pa_sw_volume_to_dB(i->volume.values[0]));
    printf("  base_volume      = %-7u (%.2f dB)   <-- hardware 0 dB anchor\n",
           i->base_volume, pa_sw_volume_to_dB(i->base_volume));
    printf("  n_volume_steps   = %u  %s\n", i->n_volume_steps,
           i->n_volume_steps == 0
             ? "(0 = arbitrary/continuous volume)"
             : "(discrete hardware steps)");

    if (i->n_volume_steps > 1)
    {
        printf("  --> HW step from base: %.3f dB per step over %u steps\n",
              pa_sw_volume_to_dB(i->base_volume) / (double)(i->n_volume_steps - 1),
              i->n_volume_steps);
    }

    printf("  active_port      = %s\n",
           i->active_port && i->active_port->name ? i->active_port->name : "(none)");

    printf("  proplist:\n");
    if (i->proplist)
    {
        void *state = NULL;
        const char *key, *val;
        while ((val = pa_proplist_iterate(i->proplist, &state)) != NULL)
        {
            key = pa_proplist_gets(i->proplist, val);
            printf("      %s = %s\n", val, key ? key : "(null)");
        }
    }
    done = 1;
}

int
main(int argc, char **argv)
{
    pa_glib_mainloop *l = pa_glib_mainloop_new(NULL);
    pa_context *ctx = pa_context_new(pa_glib_mainloop_get_api(l), "probe-sink");
    pa_operation *o = NULL;

    pa_context_connect(ctx, NULL, 0, NULL);
    for (int i = 0; i < 3000; i++)
    {
        while (g_main_context_pending(g_main_context_default()))
            g_main_context_iteration(g_main_context_default(), FALSE);
        if (pa_context_get_state(ctx) == PA_CONTEXT_READY)
            break;
        g_usleep(1000);
    }

    if (pa_context_get_state(ctx) != PA_CONTEXT_READY)
    {
        fprintf(stderr, "could not connect to pulse server\n");
        return 1;
    }

    if (argc > 1)
        o = pa_context_get_sink_info_by_name(ctx, argv[1], sink_cb, NULL);
    else
        o = pa_context_get_sink_info_by_index(ctx, 0, sink_cb, NULL);

    if (!o)
    {
        fprintf(stderr, "no such sink: %s\n",
                argc > 1 ? argv[1] : "index 0");
        return 1;
    }

    pa_operation_unref(o);
    for (int i = 0; i < 300 && !done; i++)
    {
        while (g_main_context_pending(g_main_context_default()))
            g_main_context_iteration(g_main_context_default(), FALSE);
        g_usleep(1000);
    }

    pa_context_disconnect(ctx);
    return 0;
}
