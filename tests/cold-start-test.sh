#!/bin/sh
# Cold-start ladder selection.
#
# The call state is a query, not an edge-triggered signal, so an applet that
# starts while a call is already running must land on the in-call ladder
# immediately rather than waiting for a transition it did not hear.
#
# Run from the top of the repo.
set -e

SINK=${1:-@DEFAULT_SINK@}
SO=${2:-src/.libs/volume_status_menu_item.so}
WAV=${WAV:-/tmp/long.wav}

[ -f "$WAV" ] || { echo "need $WAV"; exit 1; }

pactl set-sink-volume "$SINK" 45%
sleep 0.5

echo "A) cold start, no call stream  (expect the normal ladder)"
./tests/out/host-plugin "$SO" 2>&1 | grep -m1 harness

paplay --property=media.role=phone "$WAV" >/dev/null 2>&1 &
stream=$!
sleep 2
echo
echo "B) cold start with a live phone stream: $(( $(pactl list sink-inputs short | wc -l) )) sink input(s)"
echo "   (expect the in-call ladder, immediately)"
./tests/out/host-plugin "$SO" 2>&1 | grep -m1 harness

kill "$stream" 2>/dev/null || true
wait "$stream" 2>/dev/null || true
sleep 1

echo
echo "C) stream gone, cold start again (expect the normal ladder)"
./tests/out/host-plugin "$SO" 2>&1 | grep -m1 harness

echo
echo "sink unchanged at: $(pactl get-sink-volume "$SINK" | head -1)"
