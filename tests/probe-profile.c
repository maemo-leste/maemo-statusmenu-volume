/* Can a client be notified when the ALSA/UCM profile is switched?
 * Subscribe to everything, then flip the card profile and observe. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pulse/pulseaudio.h>

static pa_mainloop *ml;
static pa_mainloop_api *api;

static void pump(int ms) { for (int i = 0; i < ms / 50; i++) { pa_mainloop_iterate(ml, 0, NULL); usleep(50000); } }
static void succ(pa_context *c, int ok, void *u) { (void)c; (void)u; printf("    [op ok=%d]\n", ok); }

static const char *fac_name(unsigned f) {
	switch (f) {
	case PA_SUBSCRIPTION_EVENT_SINK: return "SINK";
	case PA_SUBSCRIPTION_EVENT_SOURCE: return "SOURCE";
	case PA_SUBSCRIPTION_EVENT_SINK_INPUT: return "SINK_INPUT";
	case PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT: return "SOURCE_OUTPUT";
	case PA_SUBSCRIPTION_EVENT_MODULE: return "MODULE";
	case PA_SUBSCRIPTION_EVENT_CLIENT: return "CLIENT";
	case PA_SUBSCRIPTION_EVENT_SAMPLE_CACHE: return "SAMPLE";
	case PA_SUBSCRIPTION_EVENT_SERVER: return "SERVER";
	case PA_SUBSCRIPTION_EVENT_AUTOLOAD: return "AUTOLOAD";
	case PA_SUBSCRIPTION_EVENT_CARD: return "CARD";
	default: return "?";
	}
}

static void sub_cb(pa_context *c, pa_subscription_event_type_t t, uint32_t idx, void *ud) {
	(void)c; (void)ud;
	unsigned f = t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
	unsigned k = t & PA_SUBSCRIPTION_EVENT_TYPE_MASK;
	printf("  EVENT %-13s %s (raw=0x%04x idx=%u)\n", fac_name(f),
	       k == PA_SUBSCRIPTION_EVENT_NEW ? "NEW" :
	       k == PA_SUBSCRIPTION_EVENT_CHANGE ? "CHANGE" :
	       k == PA_SUBSCRIPTION_EVENT_REMOVE ? "REMOVE" : "?", (unsigned)t, idx);
}

static void card_cb(pa_context *c, const pa_card_info *i, int eol, void *ud) {
	(void)ud;
	if (eol < 0 || eol) return;
	printf("  card %s ACTIVE='%s' profiles:", i->name,
	       i->active_profile2 ? i->active_profile2->name : "(null)");
	for (unsigned n = 0; i->profiles2 && i->profiles2[n]; n++)
		printf(" %s", i->profiles2[n]->name);
	printf("\n");
}

static void sink_cb(pa_context *c, const pa_sink_info *i, int eol, void *ud) {
	(void)ud;
	if (eol < 0 || eol) return;
	const char *pn = pa_proplist_gets(i->proplist, "device.profile.name");
	const char *pd = pa_proplist_gets(i->proplist, "device.profile.description");
	printf("  SINK %s  device.profile.name='%s' profile.description='%s'\n",
	       i->name, pn ? pn : "(none)", pd ? pd : "(none)");
}

int main(void) {
	setvbuf(stdout, NULL, _IOLBF, 0);
	ml = pa_mainloop_new();
	api = pa_mainloop_get_api(ml);
	pa_context *c = pa_context_new(api, "profilewatch");
	pa_context_connect(c, NULL, PA_CONTEXT_NOFLAGS, NULL);
	while (pa_context_get_state(c) != PA_CONTEXT_READY) { pa_mainloop_iterate(ml, 1, NULL); usleep(20000); }

	pa_context_set_subscribe_callback(c, sub_cb, NULL);
	pa_operation_unref(pa_context_subscribe(c, PA_SUBSCRIPTION_MASK_ALL, succ, NULL));
	pump(300);

	printf("=== is there a CARD subscription facility? ===\n");
	printf("  PA_SUBSCRIPTION_EVENT_CARD = 0x%04x, MASK = 0x%x  -> EXISTS\n",
	       (unsigned)PA_SUBSCRIPTION_EVENT_CARD,
	       (unsigned)PA_SUBSCRIPTION_MASK_CARD);

	printf("=== current cards/profiles ===\n");
	pa_operation_unref(pa_context_get_card_info_list(c, card_cb, NULL));
	pump(500);
	printf("=== current sinks ===\n");
	pa_operation_unref(pa_context_get_sink_info_list(c, sink_cb, NULL));
	pump(500);

	printf("\n>>> switching profile to 'output:analog-stereo' ...\n");
	pa_operation_unref(pa_context_set_card_profile_by_name(c,
		"alsa_card.pci-0000_00_05.0", "output:analog-stereo", succ, NULL));
	pump(2500);
	printf("=== sinks after switch ===\n");
	pa_operation_unref(pa_context_get_sink_info_list(c, sink_cb, NULL));
	pump(500);

	printf("\n>>> switching back to duplex ...\n");
	pa_operation_unref(pa_context_set_card_profile_by_name(c,
		"alsa_card.pci-0000_00_05.0", "output:analog-stereo+input:analog-stereo", succ, NULL));
	pump(2500);
	printf("=== sinks after switch back ===\n");
	pa_operation_unref(pa_context_get_sink_info_list(c, sink_cb, NULL));
	pump(500);

	pa_context_disconnect(c);
	pump(200);
	pa_mainloop_free(ml);
	return 0;
}
