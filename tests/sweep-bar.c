/* Sweep the real volume -> slider -> banner-bar mapping and look for plateaus,
 * reversals and clamping.  The banner draws the fill as `volume * 322.0` px
 * inside a 324 px outline, where `volume` is the slider value, so anything
 * that makes the slider value a non-monotonic or clamped function of the
 * actual sink volume shows up here as a bar that lies. */
#include "item.c"
#include <stdio.h>

int main(void) {
	gint n = 0;
	gint *steps = NULL;
	GQuark q = 0;
	double prev_slider = -1.0;

	setvbuf(stdout, NULL, _IONBF, 0);

	parse_tuning_property(DEFAULT_NORMAL_TUNING, &n, &steps, &q);
	printf("table: %d entries, top %d\n\n", n, steps[n - 1]);
	printf("%8s  %8s  %9s  %s\n", "pa_vol", "slider", "bar_px", "note");

	for (int v = 0; v <= 65536; v += 512) {
		double s = pa_vol_to_slider(v, steps, n);
		double bar = s * 322.0;
		const char *note = "";

		if (prev_slider >= 0.0) {
			if (s < prev_slider)
				note = "<-- REVERSED";
			else if (s == prev_slider && v > 0)
				note = "plateau";
		}
		if (s >= 1.0)
			note = "clamped at 1.0";

		printf("%8d  %8.4f  %9.2f  %s\n", v, s, bar, note);
		prev_slider = s;
	}

	/* What the bar looks like exactly at each table entry. */
	printf("\nat each table entry:\n");
	for (int i = 0; i < n; i++) {
		double s = pa_vol_to_slider(steps[i], steps, n);
		printf("  step %2d  vol=%6d  slider=%.4f  bar=%7.2f px\n",
		       i, steps[i], s, s * 322.0);
	}
	return 0;
}
