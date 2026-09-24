#!/bin/bash
# Trace who actually handles the volume keys on a Leste device.
#
# Run as the desktop user (needs $DISPLAY and pulse access).
# Usage: ./tests/d4-keytrace.sh
#
# Answers three questions:
#   1. Is the volume plugin actually loaded?
#   2. Does MCE emit sig_key_event_ind for the volume keys?
#   3. Does the sink volume move, and who is connected to D-Bus to do it?

set -u

VOL_READER="pactl"
command -v pactl >/dev/null 2>&1 || VOL_READER="wpctl"

vol() {
    if [ "$VOL_READER" = pactl ]; then
        pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null | head -1 | tr -s ' '
    else
        wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null
    fi
}

echo "========================================================"
echo " 1. is the volume plugin loaded, and where?"
echo "========================================================"
found=0
for p in $(pgrep -f 'hildon-status-menu|hildon-home|hildon-desktop' 2>/dev/null); do
    m=$(grep -o 'volume_status_menu_item\.so' "/proc/$p/maps" 2>/dev/null | head -1)
    if [ -n "$m" ]; then
        echo "  LOADED in pid $p ($(cat /proc/$p/comm 2>/dev/null))"
        found=1
    fi
done
[ "$found" = 0 ] && echo "  NOT LOADED in any hildon process"

echo
echo "========================================================"
echo " 2. every process with a D-Bus socket (candidate handlers)"
echo "========================================================"
if command -v ss >/dev/null 2>&1; then
    ss -xep 2>/dev/null | grep -iE 'dbus' \
        | sed -nE 's/.*pid=([0-9]+),.*/\1/p' | sort -un \
        | while read -r p; do
            printf '  %-7s %s\n' "$p" "$(cat /proc/$p/comm 2>/dev/null)"
          done
elif command -v lsof >/dev/null 2>&1; then
    lsof -U 2>/dev/null | grep -i dbus | awk '{print $1, $2}' | sort -u
else
    echo "  (neither ss nor lsof available)"
fi

echo
echo "========================================================"
echo " 3. MCE emission + sink movement"
echo "========================================================"
TRACE=/tmp/mce-keytrace.log
: > "$TRACE"
command -v dbus-monitor >/dev/null 2>&1 || { echo "  dbus-monitor missing"; exit 1; }

timeout 25 dbus-monitor --system \
    "type='signal',interface='com.nokia.mce.signal'" > "$TRACE" 2>&1 &
MON=$!
sleep 1

echo "  sink before:        $(vol)"
before=$(vol)
echo
read -r -t 10 -p "  >> press VOLUME UP now, then press Enter: " _ || true
mid=$(vol)
echo "  sink after UP:      $mid"
echo
read -r -t 10 -p "  >> press VOLUME DOWN now, then press Enter: " _ || true
after=$(vol)
echo "  sink after DOWN:    $after"

wait "$MON" 2>/dev/null

keys=$(grep -c 'sig_key_event_ind' "$TRACE" 2>/dev/null || echo 0)
echo
echo "  sig_key_event_ind emitted by MCE during the test: $keys"
grep -A2 'sig_key_event_ind' "$TRACE" 2>/dev/null | head -12 | sed 's/^/    /'

echo
echo "  verdict on the key press:"
if [ "$keys" = 0 ]; then
    echo "    MCE did NOT emit -> the key never reached MCE (different device path)"
else
    if [ "$mid" = "$before" ] && [ "$after" = "$mid" ]; then
        echo "    MCE emitted but the sink NEVER moved -> nothing handles the key"
    else
        echo "    MCE emitted AND the sink moved -> something consumed the signal"
        echo "    (cross-check against the D-Bus client list in section 2)"
    fi
fi

echo
echo "========================================================"
echo " 4. other installed volume-related binaries"
echo "========================================================"
dpkg -l 2>/dev/null | awk '/volume|hotkey|keyhandler|osso-|leste-|sphone|voicecall/ {print "  " $2 "  " $3}' | head -20
