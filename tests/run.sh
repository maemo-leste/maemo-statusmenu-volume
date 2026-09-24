#!/bin/sh
# Build and run the probes for the PipeWire / upstream-PulseAudio analysis.
# Run from the top of the repo. Requires: libpulse-dev, glib, gtk+2,
# libhildondesktop-dev, and a running audio stack (pipewire+pipewire-pulse+wireplumber,
# or pulseaudio).
set -e

cd "$(dirname "$0")/.."
SRC=src
PKG="glib-2.0 libpulse libpulse-mainloop-glib gtk+-2.0 libhildondesktop-1 \
     hildon-1 libosso dbus-glib-1 mce x11"

mkdir -p tests/out

echo "### 1. probe-pulse: sink names, proplist, ext-stream-restore, failure modes"
gcc -O0 -g -o tests/out/probe-pulse tests/probe-pulse.c \
	$(pkg-config --cflags --libs $PKG) -lm
tests/out/probe-pulse "${1:-alsa_output.0.HiFi__hw_Audio_0__sink}" || true

echo
echo "### 2. probe-events: subscription facility/type bitmask"
gcc -O0 -g -o tests/out/probe-events tests/probe-events.c \
	$(pkg-config --cflags --libs libpulse)
tests/out/probe-events || true

echo
echo "### 3. unit-math: ASan unit test of the slider/tuning math"
gcc -O0 -g -fsanitize=address -I"$SRC" -I. -o tests/out/unit-math tests/unit-math.c \
	$(pkg-config --cflags --libs $PKG) -lm
ASAN_OPTIONS=detect_leaks=0 tests/out/unit-math || true

echo
echo "### 4. host-plugin: run the real plugin and report the slider over time"
if [ -n "$DISPLAY" ]; then
	make -C . >/dev/null 2>&1 || true
	gcc -O0 -g -o tests/out/host-plugin tests/host-plugin.c \
		$(pkg-config --cflags --libs gtk+-2.0 libhildondesktop-1)
	tests/out/host-plugin "$SRC/.libs/volume_status_menu_item.so" || true
else
	echo "(skipped: no DISPLAY)"
fi

echo
echo "### 5. check-ladders: push the ladders shipped by maemo-audio through the real parser"
gcc -O0 -g -fsanitize=address -I"$SRC" -I. -o tests/out/check-ladders tests/check-ladders.c \
	$(pkg-config --cflags --libs $PKG) -lm
LADDER_CONF="${LADDER_CONF:-../maemo-audio/maemo-audio-droid4/etc/wireplumber/wireplumber.conf.d/50-maemo-volume-droid4.conf}"
if [ -f "$LADDER_CONF" ]; then
	ASAN_OPTIONS=detect_leaks=0 tests/out/check-ladders "$LADDER_CONF"
else
	echo "(skipped: $LADDER_CONF not found; point LADDER_CONF at a device drop-in)"
fi
