/* Validate the ladders shipped in a maemo-audio wireplumber drop-in by pushing
 * every one of them through the real parse_tuning_property() from src/item.c.
 *
 *   ./check-ladders <path-to-50-maemo-volume.conf>
 *
 * Built with -fsanitize=address.  For each rule it reports the step count, the
 * silence entry, the top volume and its dB, and whether the slider round-trips.
 * A ladder whose top is below PA_VOLUME_NORM is expected where the device caps
 * the boost on purpose -- the dB figure is what to eyeball. */
#include "item.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int failures = 0;

static void check(const char *port, const char *mode, const char *ladder) {
	gint n = 0;
	gint *steps = NULL;
	GQuark q = 0;
	int bad = 0;

	printf("  %-42s %-7s\n", port, mode);
	printf("      %-14s %s\n", "ladder", ladder);

	if (!parse_tuning_property(ladder, &n, &steps, &q)) {
		printf("      RESULT: parse_tuning_property() REJECTED this ladder\n");
		failures++;
		return;
	}
	if (n < 2) {
		printf("      RESULT: only %d step, unusable\n", n);
		failures++;
		g_free(steps);
		return;
	}

	printf("      %-14s %d (silence + %d content steps)\n", "entries", n, n - 1);
	printf("      %-14s %d\n", "steps[0] (silence)", steps[0]);
	printf("      %-14s %d = %+.2f dB\n", "top", steps[n - 1],
	       pa_sw_volume_to_dB(steps[n - 1]));
	printf("      %-14s %d = %+.2f dB\n", "lowest content", steps[1],
	       pa_sw_volume_to_dB(steps[1]));

	if (steps[0] != 0) {
		printf("      RESULT: steps[0] is not silence\n");
		failures++;
	}

	/* Every slider position must map to a table entry and come back. */
	for (int i = 1; i < n; i++) {
		double s = (double)i / (double)(n - 1);
		int v = slider_to_pa_vol(s, steps, n);
		double back = pa_vol_to_slider(v, steps, n);
		if (v != steps[i]) {
			printf("      RESULT: slider %.4f -> %d, table says %d\n", s, v, steps[i]);
			failures++;
			bad++;
		}
		if (fabs(back - s) > 1e-6) {
			printf("      RESULT: round trip %.4f -> %d -> %.6f\n", s, v, back);
			failures++;
			bad++;
		}
	}
	if (!bad)
		printf("      RESULT: all %d positions exact and round-trip clean\n", n - 1);

	g_free(steps);
}

int main(int argc, char **argv) {
	const char *path = (argc > 1) ? argv[1] : NULL;
	char *buf = NULL;
	size_t len = 0;
	FILE *f;
	char *p;
	char port[256] = "(unknown)";
	int rules = 0;

	setvbuf(stdout, NULL, _IONBF, 0);

	if (!path) {
		fprintf(stderr, "usage: %s <50-maemo-volume.conf>\n", argv[0]);
		return 2;
	}

	f = fopen(path, "rb");
	if (!f) {
		perror(path);
		return 2;
	}
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = g_malloc(len + 1);
	if (fread(buf, 1, len, f) != len) {
		fprintf(stderr, "short read\n");
		fclose(f);
		return 2;
	}
	buf[len] = '\0';
	fclose(f);

	printf("Checking %s\n\n", path);

	/* Walk line by line, remembering the most recent node.name pattern so each
	 * tuning property is reported against the rule it lives in. */
	p = buf;
	while (p && *p) {
		char *eol = strchr(p, '\n');
		if (eol)
			*eol = '\0';

		char *nn = strstr(p, "node.name");
		if (nn) {
			char *q1 = strchr(nn, '"');
			if (q1) {
				char *q2 = strchr(q1 + 1, '"');
				if (q2) {
					size_t l = (size_t)(q2 - q1 - 1);
					if (l >= sizeof(port))
						l = sizeof(port) - 1;
					memcpy(port, q1 + 1, l);
					port[l] = '\0';
				}
			}
		}

		char *tp = strstr(p, "x-maemo.volume.tuning");
		if (tp) {
			int incall = strstr(tp, ".incall") != NULL;
			char *q1 = strchr(tp, '"');
			if (q1) {
				char *q2 = strchr(q1 + 1, '"');
				if (q2) {
					*q2 = '\0';
					check(port, incall ? "incall" : "normal", q1 + 1);
					rules++;
					*q2 = '"';
				}
			}
		}

		if (!eol)
			break;
		p = eol + 1;
	}

	printf("\n%d ladders checked, %d problem(s)\n", rules, failures);
	g_free(buf);
	return failures ? 1 : 0;
}
