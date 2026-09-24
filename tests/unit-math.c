/* Unit-test the pure math/config helpers from src/item.c by including it.
 * Built with -fsanitize=address to surface out-of-bounds accesses. */
#include "item.c"
#include <stdio.h>

static void dump_steps(const char *tag, gint *steps, gint n) {
	printf("%s n=%d steps=[", tag, n);
	for (int i = 0; i < n; i++) printf("%d%s", steps[i], i + 1 < n ? "," : "");
	printf("]\n");
}

int main(void) {
	SoundsStatusMenuItemPrivate p;
	setvbuf(stdout, NULL, _IONBF, 0);
	memset(&p, 0, sizeof(p));

	apply_default_tuning(&p);
	dump_steps("normal", p.normal_volume_steps, p.normal_volume_num_steps);
	dump_steps("incall", p.incall_volume_steps, p.incall_volume_num_steps);

	printf("\n-- default table shape checks --\n");
	printf("  normal: num_steps=%d (want 21 = silence + 20 steps) %s\n",
	       p.normal_volume_num_steps,
	       p.normal_volume_num_steps == 21 ? "OK" : "MISMATCH");
	printf("  incall: num_steps=%d (want 11 = silence + 10 steps) %s\n",
	       p.incall_volume_num_steps,
	       p.incall_volume_num_steps == 11 ? "OK" : "MISMATCH");
	printf("  normal top = %d (want 65536 = 0.00 dB) %s\n",
	       p.normal_volume_steps[p.normal_volume_num_steps - 1],
	       p.normal_volume_steps[p.normal_volume_num_steps - 1] == 65536 ? "OK" : "MISMATCH");
	printf("  incall top = %d (want 65536 = 0.00 dB) %s\n",
	       p.incall_volume_steps[p.incall_volume_num_steps - 1],
	       p.incall_volume_steps[p.incall_volume_num_steps - 1] == 65536 ? "OK" : "MISMATCH");
	printf("  slider at max maps to top? ");
	{
		int v = slider_to_pa_vol(1.0, p.normal_volume_steps, p.normal_volume_num_steps);
		printf("slider_to_pa_vol(1.0)=%d %s\n", v,
		       v == p.normal_volume_steps[p.normal_volume_num_steps - 1] ? "OK" : "MISMATCH");
	}
	printf("  dB span normal: %.2f dB .. %.2f dB\n",
	       pa_sw_volume_to_dB(p.normal_volume_steps[1]),
	       pa_sw_volume_to_dB(p.normal_volume_steps[p.normal_volume_num_steps - 1]));
	printf("  dB span incall: %.2f dB .. %.2f dB\n",
	       pa_sw_volume_to_dB(p.incall_volume_steps[1]),
	       pa_sw_volume_to_dB(p.incall_volume_steps[p.incall_volume_num_steps - 1]));

	printf("\n-- pa_vol_to_slider() OOB probe (reads steps[n]) --\n");
	for (int v = 0; v <= 65536; v += 8192)
		printf("  vol=%5d -> slider=%.4f\n", v, pa_vol_to_slider(v, p.normal_volume_steps, p.normal_volume_num_steps));

	printf("\n-- slider_to_pa_vol round trip --\n");
	for (double s = 0.0; s <= 1.001; s += 0.1) {
		int v = slider_to_pa_vol(s, p.normal_volume_steps, p.normal_volume_num_steps);
		printf("  slider=%.2f -> vol=%5d -> back=%.4f\n", s, v, pa_vol_to_slider(v, p.normal_volume_steps, p.normal_volume_num_steps));
	}

	printf("\n-- parse_tuning_property() with NULL property (pipewire case) --\n");
	GQuark q = 0;
	gint ns = 0, *st = NULL;
	gboolean changed = parse_tuning_property(NULL, &ns, &st, &q);
	printf("  changed=%d num_steps=%d steps[0]=%d\n", changed, ns, st ? st[0] : -1);
	if (changed && ns <= 1)
		printf("  !! num_steps<=1 -> slider_to_pa_vol()/pa_vol_to_slider() g_assert(num_steps>1) ABORTS\n");

	printf("\n-- parse_tuning_property() with a new-format (centibel) string --\n");
	q = 0; ns = 0; st = NULL;
	changed = parse_tuning_property("-4000,-2000,-1000,0", &ns, &st, &q);
	dump_steps("  parsed", st, ns);
	printf("  changed=%d num_steps=%d (want 5 = silence + 4 steps)\n", changed, ns);
	printf("  dB of steps: ");
	for (int i = 1; i < ns; i++) printf("%.2f ", pa_sw_volume_to_dB(st[i]));
	printf("\n");

	printf("\n-- old Nokia 'alsa_value:dB' form must now be rejected --\n");
	q = 0; ns = 0; st = NULL;
	changed = parse_tuning_property("1:-5850,31:-4350,118:0", &ns, &st, &q);
	printf("  changed=%d (want 0: '1:-5850' is not a number)\n", changed);

	printf("\n-- non-ascending entry is skipped, not a divide-by-zero --\n");
	q = 0; ns = 0; st = NULL;
	changed = parse_tuning_property("-4000,-4000,-2000,0", &ns, &st, &q);
	printf("  changed=%d num_steps=%d (want 4: duplicate dropped)\n", changed, ns);
	for (int i = 0; i < ns; i++)
		printf("    [%d]=%d\n", i, st[i]);

	printf("\n-- slider_volume_decrease_step with num_steps==1 (assert order bug) --\n");
	printf("   (code checks num_steps==1 AFTER calling slider_to_pa_vol which asserts >1)\n");

	printf("\n-- parse_tuning_property(): valid then NULL (sink switch) --\n");
	q = 0; ns = 0; st = NULL;
	parse_tuning_property("-4000,-2000,0", &ns, &st, &q);
	printf("  after valid: num_steps=%d quark=%u\n", ns, (unsigned)q);
	changed = parse_tuning_property(NULL, &ns, &st, &q);
	printf("  after NULL : changed=%d num_steps=%d  (must not crash)\n", changed, ns);

	printf("\n-- droid4 in-call table: top 10 cpcap positions (ctrl 6..15) --\n");
	{
		const char *d4 = "-2700,-2400,-2100,-1800,-1500,-1200,-900,-600,-300,0";
		/* predicted by tests/gen-tuning-table.py from the cpcap TLV */
		const int want[10] = { 23253, 26090, 29274, 32846, 36854,
		                       41350, 46396, 52057, 58409, 65536 };
		q = 0; ns = 0; st = NULL;
		changed = parse_tuning_property(d4, &ns, &st, &q);
		dump_steps("  d4 incall", st, ns);
		printf("  changed=%d num_steps=%d (want 11 = silence + 10 steps) %s\n",
		       changed, ns, (changed && ns == 11) ? "OK" : "MISMATCH");

		printf("  step-by-step vs predicted hardware mapping:\n");
		for (int i = 0; i < 10; i++) {
			int got = st[i + 1];
			int hw_dB = (int) lround(pa_sw_volume_to_dB(got) + 12.0);
			int ctrl  = (hw_dB + 33) / 3;
			printf("    [%2d] cB=%6d  got=%6d  want=%6d  %s   hw=%+3d dB ctrl=%2d\n",
			       i + 1, -2700 + i * 300, got, want[i],
			       got == want[i] ? "OK" : "MISMATCH", hw_dB, ctrl);
		}

		printf("  base_volume 41350 (hw 0 dB) -> slider %.4f (want 0.6000, entry 6 of 10)\n",
		       pa_vol_to_slider(41350, st, ns));
		printf("  slider at max -> %d (want 65536)\n",
		       slider_to_pa_vol(1.0, st, ns));
		printf("  slider at min content step -> %d (want %d)\n",
		       slider_to_pa_vol(1.0 / 10.0, st, ns), want[0]);
		printf("  round trip at each logical step:\n");
		for (int i = 1; i <= 10; i++) {
			double s = i / 10.0;
			int v = slider_to_pa_vol(s, st, ns);
			printf("    slider=%.1f -> vol=%6d -> back=%.4f\n",
			       s, v, pa_vol_to_slider(v, st, ns));
		}
	}

	printf("\n-- droid4 HiFi normal: all hardware positions to 0 dB + one boost (ctrl 0..12) --\n");
	{
		const char *d4 = "-4500,-4200,-3900,-3600,-3300,-3000,-2700,-2400,"
		                 "-2100,-1800,-1500,-1200,-900";
		/* predicted by tests/gen-tuning-table.py --ctrl-min 0 --ctrl-max 12
		 * --tlv-max 12 --steps 13 from the cpcap HiFi TLV */
		const int want[13] = { 11654, 13076, 14672, 16462, 18471, 20724,
		                       23253, 26090, 29274, 32846, 36854, 41350,
		                       46396 };
		int bad = 0;
		q = 0; ns = 0; st = NULL;
		changed = parse_tuning_property(d4, &ns, &st, &q);
		printf("  changed=%d num_steps=%d (want 14 = silence + 13 steps) %s\n",
		       changed, ns, (changed && ns == 14) ? "OK" : "MISMATCH");

		for (int i = 0; i < 13; i++) {
			int got = st[i + 1];
			int hw_dB = (int) lround(pa_sw_volume_to_dB(got) + 12.0);
			double ctrl_d = (hw_dB + 33.0) / 3.0;
			int ok = (got == want[i]) && (fabs(ctrl_d - rint(ctrl_d)) < 1e-9);
			if (!ok) bad++;
			printf("    [%2d] cB=%6d got=%6d want=%6d  hw=%+3d dB ctrl=%5.2f  %s\n",
			       i + 1, -4500 + i * 300, got, want[i], hw_dB, ctrl_d,
			       ok ? "OK" : "MISMATCH");
		}
		printf("  entries: %d/13 exact hardware positions, %d mismatched\n",
		       13 - bad, bad);

		printf("  base_volume 41350 (hw 0 dB, ctrl 11) -> slider %.7f (want %.7f)\n",
		       pa_vol_to_slider(41350, st, ns), 12.0 / 13.0);
		printf("  slider at max -> %d (want 46396 = +3 dB boost cap, NOT 65536)\n",
		       slider_to_pa_vol(1.0, st, ns));
		printf("  slider at first content step -> %d (want %d = -33 dB)\n",
		       slider_to_pa_vol(1.0 / 13.0, st, ns), want[0]);
		printf("  above the ladder: 65536 -> slider %.4f (clamps to 1.0)\n",
		       pa_vol_to_slider(65536, st, ns));
		printf("  round trip at each of the 13 logical steps:\n");
		for (int i = 1; i <= 13; i++) {
			double s = i / 13.0;
			int v = slider_to_pa_vol(s, st, ns);
			double back = pa_vol_to_slider(v, st, ns);
			printf("    slider=%.7f -> vol=%6d -> back=%.7f %s\n",
			       s, v, back, fabs(back - s) < 1e-9 ? "OK" : "DRIFT");
		}
	}

	return 0;
}
