/* Simulate the real volume-up keypress loop exactly as prop_sink_info_cb drives
 * it, starting from silence, and report where the banner bar fails to advance.
 *
 *   v   = sink volume read back
 *   s   = pa_vol_to_slider(v)            -> what the slider shows
 *   bar = s * 322.0                      -> what the banner fill is drawn as
 *
 * The reported symptom is: slider advances one position per press, but the
 * bar sometimes stays put. */
#include "item.c"
#include <stdio.h>

/* Copy of slider_volume_increase_step() with the table passed in explicitly. */
static double
inc_step(double volume, gint *steps, gint num_steps)
{
	int pa_vol = slider_to_pa_vol(volume, steps, num_steps) + 1;
	int i;

	for (i = 0; i < num_steps - 1; i++)
		if (pa_vol < steps[i])
			break;

	return i / (double)(num_steps - 1);
}

int main(void) {
	gint n = 0;
	gint *steps = NULL;
	GQuark q = 0;
	int v = 0;
	double prev_bar = -1.0;
	double prev_slider = -1.0;

	setvbuf(stdout, NULL, _IONBF, 0);
	parse_tuning_property(DEFAULT_NORMAL_TUNING, &n, &steps, &q);

	printf("starting from silence, %d presses\n\n", n + 2);
	printf("%-3s %8s %9s %8s  %s\n", "#", "sink_vol", "slider", "bar_px", "what moved");

	for (int press = 1; press <= n + 2; press++) {
		double s = pa_vol_to_slider(v, steps, n);
		double s2 = inc_step(s, steps, n);
		int v2 = slider_to_pa_vol(s2, steps, n);
		double bar = s * 322.0;
		char note[96];

		snprintf(note, sizeof(note),
		         "slider %s, bar %s",
		         (prev_slider < 0.0) ? "-" :
		         (s > prev_slider ? "+moved" : "=STALLED"),
		         (prev_bar < 0.0) ? "-" :
		         (bar > prev_bar ? "+moved" : "=STALLED"));

		printf("%-3d %8d %9.4f %8.2f  %s\n", press, v, s, bar, note);

		if (v2 == v) {
			printf("    -> no further progress (sink stuck at %d)\n", v);
			break;
		}
		prev_bar = bar;
		prev_slider = s;
		v = v2;
	}
	return 0;
}
