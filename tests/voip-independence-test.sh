#!/bin/sh
# VOIP / media volume independence.
#
# Before E the in-call slider wrote the SINK, so adjusting a call also
# moved media volume with it. Now the slider writes the call-role stream
# and the sink is left alone.
#
# Run from the top of the repo.
set -e

SINK=${1:-@DEFAULT_SINK@}
SO=${2:-src/.libs/volume_status_menu_item.so}
WAV=${WAV:-/tmp/long.wav}

[ -f "$WAV" ] || { echo "need $WAV"; exit 1; }

sink_vol() { pactl get-sink-volume "$SINK" | head -1 | grep -o '[0-9]*%' | head -1; }

stream_vol() {
	# pactl has no get-sink-input-volume in this version; parse the listing.
	pactl list sink-inputs 2>/dev/null | grep -m1 'Volume:' | grep -o '[0-9]*%' | head -1
}

pactl set-sink-volume "$SINK" 45%
sleep 0.5
echo "media sink at start:            $(sink_vol)"

paplay --property=media.role=phone "$WAV" >/dev/null 2>&1 &
stream=$!
sleep 2
echo "call stream at start:           $(stream_vol)"
echo
echo "Driving the slider while the call is up. The sink must not move."
echo

./tests/out/slider-drive "$SO" 0.25 0.50 0.75 >/tmp/e-drive.log 2>&1 &
drive=$!

for step in 1 2 3; do
	sleep 2
	printf '  after %-9s sink=%-6s stream=%s\n' \
		"$(grep SET /tmp/e-drive.log | tail -1)" "$(sink_vol)" "$(stream_vol)"
	sleep 1
done

wait "$drive" 2>/dev/null || true

echo
echo "final media sink:               $(sink_vol)   (want 45% -- untouched)"
echo "final call stream:              $(stream_vol)"

kill "$stream" 2>/dev/null || true
wait "$stream" 2>/dev/null || true
sleep 1

echo
echo "after the call ends, cold start should show the untouched sink level"
echo "  media sink:                   $(sink_vol)"
./tests/out/host-plugin "$SO" 2>&1 | grep -m1 harness | sed 's/^/  /'
