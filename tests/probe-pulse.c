/* Probe of the libpulse calls used by maemo-statusmenu-volume, run against
 * pipewire-pulse (and comparable against real pulseaudio). */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <glib.h>
#include <pulse/pulseaudio.h>
#include <pulse/ext-stream-restore.h>
#include <pulse/ext-device-restore.h>

static pa_mainloop *ml;
static pa_mainloop_api *api;
static int pending = 0;

static void step_done(pa_context *c, int success, void *ud) {
	(void)c; (void)ud;
	printf("    [op success=%d]\n", success);
}
static void pump(int ms) { for (int i = 0; i < ms / 50; i++) { pa_mainloop_iterate(ml, 0, NULL); usleep(50000); } }
#define wait_ops() pump(700)

static void si_cb(pa_context *c, const pa_sink_info *i, int eol, void *ud) {
	(void)ud;
	if (eol < 0) { printf("  sink_info: ERROR %s\n", pa_strerror(pa_context_errno(c))); return; }
	if (eol) return;
	printf("  sink name=%s channels=%u avg=%u base=%u mute=%d driver=%s\n",
	       i->name, i->channel_map.channels, pa_cvolume_avg(&i->volume),
	       i->base_volume, i->mute, i->driver);
	const char *t = pa_proplist_gets(i->proplist, "x-maemo.alsa_sink.alt_mixer_tuning");
	printf("  x-maemo.alsa_sink.alt_mixer_tuning = %s\n", t ? t : "(NULL)");
	printf("  all proplist keys:");
	void *st = NULL; const char *k;
	while ((k = pa_proplist_iterate(i->proplist, &st)) != NULL) printf(" %s", k);
	printf("\n");
}

static void sr_test_cb(pa_context *c, uint32_t version, void *ud) {
	(void)ud;
	printf("ext_stream_restore_test: version=%u %s\n", version,
	       version == 0 ? "(extension NOT available)" : "");
}

static void sr_read_cb(pa_context *c, const pa_ext_stream_restore_info *info, int eol, void *ud) {
	(void)ud;
	if (eol < 0) { printf("  stream_restore_read: ERROR %s\n", pa_strerror(pa_context_errno(c))); return; }
	if (eol) return;
	printf("  stream_restore entry: name='%s' ch=%u vol=%u mute=%d device='%s'\n",
	       info->name, info->channel_map.channels, pa_cvolume_avg(&info->volume),
	       info->mute, info->device ? info->device : "(null)");
}

static void dr_test_cb(pa_context *c, uint32_t version, void *ud) {
	(void)ud;
	printf("ext_device_restore_test: version=%u %s\n", version,
	       version == 0 ? "(NOT available)" : "");
}

static void dr_read_cb(pa_context *c, const pa_ext_device_restore_info *info, int eol, void *ud) {
	(void)ud;
	if (eol < 0) { printf("  device_restore_read: ERROR %s\n", pa_strerror(pa_context_errno(c))); return; }
	if (eol) return;
	printf("  device_restore entry: type=%u index=%u n_formats=%u\n",
	       (unsigned)info->type, info->index, (unsigned)info->n_formats);
}

static void soi_cb(pa_context *c, const pa_server_info *i, void *ud) {
	(void)ud;
	if (!i) { printf("server_info ERROR: %s\n", pa_strerror(pa_context_errno(c))); return; }
	printf("server: %s %s default_sink=%s default_source=%s\n",
	       i->server_name, i->server_version, i->default_sink_name, i->default_source_name);
}

static void sub_cb(pa_context *c, pa_subscription_event_type_t t, uint32_t idx, void *ud) {
	(void)c; (void)ud;
	printf("  SUB event raw=0x%04x facility=0x%02x type=0x%02x idx=%u | old-style check: CHANGE?%s SINK?%s\n",
	       (unsigned)t, (unsigned)(t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK),
	       (unsigned)(t & PA_SUBSCRIPTION_EVENT_TYPE_MASK), idx,
	       (t == PA_SUBSCRIPTION_EVENT_CHANGE) ? "yes" : "no",
	       (t == PA_SUBSCRIPTION_EVENT_SINK) ? "yes" : "no");
}

/* mimic parse_tuning_property() from item.c with NULL input */
static void probe_parse_null(void) {
	GQuark q = g_quark_from_string(NULL);
	printf("g_quark_from_string(NULL) -> %u (no crash)\n", (unsigned)q);
}

int main(int argc, char **argv) {
	const char *bogus = argc > 1 ? argv[1] : "alsa_output.0.HiFi__hw_Audio_0__sink";

	setvbuf(stdout, NULL, _IOLBF, 0);
	ml = pa_mainloop_new();
	api = pa_mainloop_get_api(ml);
	pa_context *c = pa_context_new(api, "patest");
	pa_context_connect(c, NULL, PA_CONTEXT_NOFLAGS, NULL);

	while (pa_context_get_state(c) != PA_CONTEXT_READY) {
		if (pa_context_get_state(c) == PA_CONTEXT_FAILED || pa_context_get_state(c) == PA_CONTEXT_TERMINATED) {
			printf("connect failed: %s\n", pa_strerror(pa_context_errno(c)));
			return 1;
		}
		pa_mainloop_iterate(ml, 1, NULL);
	}
	printf("context READY\n");
	probe_parse_null();

	pa_operation *o;
	pending = 1; pa_context_get_server_info(c, soi_cb, NULL); wait_ops();

	printf("--- ext tests ---\n");
	pending = 1; pa_ext_stream_restore_test(c, sr_test_cb, NULL); wait_ops();
	pending = 1; pa_ext_device_restore_test(c, dr_test_cb, NULL); wait_ops();

	printf("--- stream_restore_read (all) ---\n");
	pending = 1; pa_ext_stream_restore_read(c, sr_read_cb, NULL); wait_ops();

	printf("--- device_restore_read_formats_all ---\n");
	pending = 1; pa_ext_device_restore_read_formats_all(c, dr_read_cb, NULL); wait_ops();

	printf("--- sink info by bogus name (%s) ---\n", bogus);
	pending = 1; pa_context_get_sink_info_by_name(c, bogus, si_cb, NULL); wait_ops();
	printf("  errno after bogus lookup: %s\n", pa_strerror(pa_context_errno(c)));

	printf("--- sink info by NULL name ---\n");
	pending = 1; pa_context_get_sink_info_by_name(c, NULL, si_cb, NULL); wait_ops();
	printf("  errno: %s\n", pa_strerror(pa_context_errno(c)));

	printf("--- set_sink_volume_by_name with channels=0 (possible set_volume() state) ---\n");
	{
		pa_cvolume cv; memset(&cv, 0, sizeof(cv));
		pending = 1;
		o = pa_context_set_sink_volume_by_name(c, "alsa_output.pci-0000_00_05.0.analog-stereo", &cv, step_done, NULL);
		if (!o) printf("  no operation: %s\n", pa_strerror(pa_context_errno(c)));
		wait_ops();
	}

	printf("--- write stream-restore entry 'sink-input-by-media-role:x-maemo' (save=TRUE) ---\n");
	{
		pa_ext_stream_restore_info info;
		memset(&info, 0, sizeof(info));
		pa_channel_map_init_mono(&info.channel_map);
		pa_cvolume_set(&info.volume, 1, 30000);
		info.name = "sink-input-by-media-role:x-maemo";
		info.device = NULL;
		info.mute = 0;
		pending = 1;
		o = pa_ext_stream_restore_write(c, PA_UPDATE_REPLACE, &info, 1, TRUE, step_done, NULL);
		if (!o) printf("  write failed: %s\n", pa_strerror(pa_context_errno(c)));
		wait_ops();
	}
	printf("--- read back stream-restore ---\n");
	pending = 1; pa_ext_stream_restore_read(c, sr_read_cb, NULL); wait_ops();

	printf("--- subscribe to all events, then change sink volume (5s) ---\n");
	pa_context_set_subscribe_callback(c, sub_cb, NULL);
	pending = 1;
	o = pa_context_subscribe(c, PA_SUBSCRIPTION_MASK_ALL, step_done, NULL);
	wait_ops();
	{
		pa_cvolume cv;
		pa_cvolume_set(&cv, 2, 40000);
		pending = 1;
		pa_context_set_sink_volume_by_name(c, "alsa_output.pci-0000_00_05.0.analog-stereo", &cv, step_done, NULL);
		wait_ops();
	}
	/* keep iterating for 3 more seconds to catch events */
	pump(3000);

	pa_context_disconnect(c);
	pa_mainloop_free(ml);
	return 0;
}
