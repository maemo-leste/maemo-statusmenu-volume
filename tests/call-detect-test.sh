#!/bin/sh
# Exercise the media-role call detector.
#
# The applet chooses its ladder from "is any sink input carrying a call
# role", so this starts and stops streams with different roles and shows the
# resulting slider position, the call-state transitions, and the actual sink
# volume (to prove the ladder switch does not itself move the sink).
#
# Run from the top of the repo, with a display and a running audio stack.
set -e

SINK=${1:-@DEFAULT_SINK@}
SO=${2:-src/.libs/volume_status_menu_item.so}
LOG=/tmp/call-detect.log
WAV=${WAV:-/tmp/long.wav}

[ -f "$WAV" ] || { echo "need $WAV (any ~25s file)"; exit 1; }

gcc -O0 -g -o tests/out/harness-watch tests/harness-watch.c \
	$(pkg-config --cflags --libs gtk+-2.0 libhildondesktop-1)

pactl set-sink-volume "$SINK" 45%
sleep 0.5
echo "start: $(pactl get-sink-volume "$SINK" | head -1)"

G_MESSAGES_DEBUG=all ./tests/out/harness-watch "$SO" >"$LOG" 2>&1 &
WATCH=$!
sleep 4

phase() {
	role=$1
	echo "--- phase: media.role=$role ---"
	paplay --property="media.role=$role" "$WAV" &
	stream=$!
	sleep 6
	kill "$stream" 2>/dev/null || true
	wait "$stream" 2>/dev/null || true
	sleep 4
}

phase phone
phase Communication
phase music
phase x-maemo

kill "$WATCH" 2>/dev/null || true

echo
echo "=== call state transitions ==="
grep -o "call state is now [a-z]*" "$LOG" || echo "(none)"
echo
echo "=== slider over time (one sample per second) ==="
grep "^\[watch\]" "$LOG" | sed 's/.*slider=//' | awk 'NR%2==1 {printf "%s ", $1} END {print ""}'
echo
echo "=== sink volume after all phases (should still be 45%) ==="
pactl get-sink-volume "$SINK" | head -1
