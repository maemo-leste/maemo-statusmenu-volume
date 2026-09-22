#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pulse/pulseaudio.h>

static pa_mainloop *ml;
static pa_mainloop_api *api;

static void pump(int ms) { for (int i = 0; i < ms/50; i++) { pa_mainloop_iterate(ml, 0, NULL); usleep(50000); } }

static void sub_cb(pa_context *c, pa_subscription_event_type_t t, uint32_t idx, void *ud) {
	(void)c; (void)ud;
	printf("  raw=0x%04x facility=0x%02x type=0x%02x idx=%u  |  applet check: ==CHANGE?%s  ==SINK?%s\n",
	       (unsigned)t, (unsigned)(t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK),
	       (unsigned)(t & PA_SUBSCRIPTION_EVENT_TYPE_MASK), idx,
	       (t == PA_SUBSCRIPTION_EVENT_CHANGE) ? "YES" : "no",
	       (t == PA_SUBSCRIPTION_EVENT_SINK) ? "YES" : "no");
}

static void succ(pa_context *c, int ok, void *ud) { (void)c; (void)ud; }

int main(void) {
	ml = pa_mainloop_new();
	api = pa_mainloop_get_api(ml);
	pa_context *c = pa_context_new(api, "subtest");
	pa_context_connect(c, NULL, PA_CONTEXT_NOFLAGS, NULL);
	while (pa_context_get_state(c) != PA_CONTEXT_READY) { pa_mainloop_iterate(ml, 1, NULL); usleep(20000); }

	pa_context_set_subscribe_callback(c, sub_cb, NULL);
	pa_operation_unref(pa_context_subscribe(c, PA_SUBSCRIPTION_MASK_ALL, succ, NULL));
	pump(300);

	printf("PA_SUBSCRIPTION_EVENT_SINK=0x%04x CHANGE=0x%04x NEW=0x%04x REMOVE=0x%04x\n",
	       (unsigned)PA_SUBSCRIPTION_EVENT_SINK, (unsigned)PA_SUBSCRIPTION_EVENT_CHANGE,
	       (unsigned)PA_SUBSCRIPTION_EVENT_NEW, (unsigned)PA_SUBSCRIPTION_EVENT_REMOVE);

	printf("--- change sink volume (expect SINK|CHANGE) ---\n");
	pa_cvolume cv; pa_cvolume_set(&cv, 2, 33000);
	pa_operation_unref(pa_context_set_sink_volume_by_name(c, "alsa_output.pci-0000_00_05.0.analog-stereo", &cv, succ, NULL));
	pump(700);

	printf("--- create a stream (expect SINK_INPUT|NEW) ---\n");
	pa_sample_spec ss; ss.format = PA_SAMPLE_S16LE; ss.rate = 44100; ss.channels = 2;
	pa_proplist *pl = pa_proplist_new();
	pa_proplist_sets(pl, PA_PROP_MEDIA_ROLE, "x-maemo");
	pa_stream *s = pa_stream_new_with_proplist(c, "probe-stream", &ss, NULL, pl);
	pa_stream_connect_playback(s, NULL, NULL, PA_STREAM_START_CORKED, NULL, NULL);
	pump(900);
	printf("--- disconnect stream (expect SINK_INPUT|REMOVE) ---\n");
	pa_stream_disconnect(s);
	pump(900);
	pa_stream_unref(s);

	pa_context_disconnect(c);
	pump(200);
	pa_mainloop_free(ml);
	return 0;
}
