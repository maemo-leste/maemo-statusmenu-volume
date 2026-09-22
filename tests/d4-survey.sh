#!/bin/sh
# Read-only audio survey for droid4 (maemo leste).
#
# Gathers everything needed to build per-device x-maemo.volume.tuning ladders:
# cards, codecs, UCM profiles/ports, PipeWire node properties, hardware dB
# ranges, jack state, Bluetooth and HDMI.
#
# Makes no configuration changes and restarts nothing. Safe to run any time.
#
#   sh d4-survey.sh                 # writes /tmp/d4-audio-report.txt
#   sh d4-survey.sh myreport.txt
#
# Run as the desktop user. Sections that need root are marked and skipped
# rather than failing the whole run.

OUT="${1:-/tmp/d4-audio-report.txt}"

{
echo "############################################################"
echo "# droid4 audio survey"
echo "# generated: $(date -Is)"
echo "# host:      $(uname -n)"
echo "############################################################"

echo
echo "==================== 1. versions ===================="
uname -a
grep -E '^(PRETTY_NAME|VERSION_ID)' /etc/os-release 2>/dev/null
for p in pipewire pipewire-pulse pipewire-bluez5 wireplumber \
         pulseaudio libpulse0 libasound2 alsa-utils \
         firmware-twl6040 maemo-audio leste-config-common; do
    v=$(dpkg-query -W -f='${Package} ${Version}\n' "$p" 2>/dev/null)
    [ -n "$v" ] && echo "  $v"
done
pipewire --version 2>/dev/null | sed 's/^/  pw: /' | head -4
wireplumber --version 2>/dev/null | sed 's/^/  wp: /' | head -4

echo
echo "==================== 2. ALSA cards ===================="
cat /proc/asound/cards 2>/dev/null
echo "-- aplay -l --"
aplay -l 2>&1
echo "-- arecord -l --"
arecord -l 2>&1

echo
echo "==================== 3. codec details ===================="
for c in /proc/asound/card*/codec#*; do
    [ -e "$c" ] || continue
    echo "--- $c ---"
    head -12 "$c" 2>/dev/null
done

echo
echo "==================== 4. UCM verbs (listing only) ===================="
echo "NOTE: deliberately does NOT 'set _verb'. Raw alsaucm bypasses PipeWire,"
echo "      which owns the UCM device via alsa-monitor, and switching the verb"
echo "      underneath it desyncs the node/profile state we are trying to read."
echo "      Profile switching is exercised through pactl set-card-profile in"
echo "      d4-snap.sh instead, which is the path the applet actually sees."
if command -v alsaucm >/dev/null 2>&1; then
    for card in $(ls /sys/class/sound/ 2>/dev/null | grep '^card[0-9]*$'); do
        echo "--- $card ---"
        alsaucm -c "$card" list _verb 2>&1 | sed 's/^/    /'
    done
else
    echo "  alsaucm not installed; 'pactl list cards -v' below is the fallback"
fi

echo
echo "==================== 5. pactl info ===================="
pactl info 2>&1

echo
echo "==================== 6. pactl list cards (profiles + ports) ===================="
echo "(this is the key section: UCM profiles and ports as PipeWire sees them)"
echo "NOTE: 'pactl list cards -v' is NOT valid syntax - pactl rejects -v for"
echo "      sinks/sources/cards and prints a usage line instead of data."
pactl list cards 2>&1

echo
echo "==================== 7. pactl list sinks ===================="
pactl list sinks 2>&1

echo
echo "==================== 8. pactl list sources ===================="
pactl list sources 2>&1

echo
echo "==================== 9. hardware dB ranges per control ===================="
echo "(the | dBscale- lines are what the ladders must be built from)"
for c in $(seq 0 7); do
    [ -e "/proc/asound/card$c" ] || continue
    echo "--- amixer -c $c contents ---"
    amixer -c "$c" contents 2>&1 | grep -E "^numid|dBscale|values|name=" | sed 's/^/  /'
done

echo
echo "==================== 10. jack / switch state ===================="
for sw in /sys/class/sound/jack/*; do
    [ -e "$sw" ] || continue
    echo "--- $sw ---"
    for f in "$sw"/*; do
        [ -f "$f" ] && echo "  $(basename "$f") = $(cat "$f" 2>/dev/null)"
    done
done
echo "-- amixer jack controls --"
for c in $(seq 0 7); do
    [ -e "/proc/asound/card$c" ] || continue
    amixer -c "$c" contents 2>&1 | grep -iE -A3 "jack|switch" | grep -iE "name=|values" | sed "s/^/  card$c: /"
done

echo
echo "==================== 11. PipeWire nodes (filtered) ===================="
if command -v pw-dump >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1; then
    pw-dump 2>/dev/null > /tmp/d4-pwdump.json
    python3 - <<'PY'
import json
keys = ('node.name','node.description','node.port','media.class','media.role',
        'device.name','device.description','device.form-factor','device.bus',
        'device.api','device.product.name','device.string','alsa.card_name',
        'alsa.driver_name','api.alsa.card.name','api.acp.device-port',
        'x-maemo.volume.tuning','x-maemo.volume.tuning.incall')

def parts(obj):
    """info/props shape differs between pw-dump versions: info is usually at
    the top level and props is a dict, but handle the nested/list variants too
    so a silent empty result cannot be mistaken for 'no devices'."""
    info = obj.get('info')
    if not isinstance(info, dict):
        info = obj.get('object', {})
        if isinstance(info, dict):
            info = info.get('info', {})
    if not isinstance(info, dict):
        return {}, {}
    props = info.get('props') or {}
    if isinstance(props, list):
        merged = {}
        for p in props:
            if isinstance(p, dict):
                merged.update(p)
        props = merged
    if not isinstance(props, dict):
        props = {}
    params = info.get('params') or {}
    if not isinstance(params, dict):
        params = {}
    return props, params

try:
    d = json.load(open('/tmp/d4-pwdump.json'))
except Exception as e:
    print("  pw-dump parse failed:", e); raise SystemExit

shown = 0
for obj in d:
    props, params = parts(obj)
    mc = props.get('media.class', '')
    if mc not in ('Audio/Device', 'Audio/Sink', 'Audio/Source'):
        continue
    shown += 1
    print(f"  [{mc}] id={obj.get('id')} {obj.get('type','')}")
    for k in keys:
        if k in props:
            print(f"      {k} = {props[k]}")
    for pname, v in params.items():
        if pname in ('EnumProfile', 'EnumPort', 'Profile', 'Ports'):
            print(f"      params.{pname}: {json.dumps(v)[:1200]}")
print(f"  ({shown} audio device/sink/source nodes matched)")
PY
else
    echo "  pw-dump or python3 missing"
fi

echo
echo "==================== 12. any x-maemo props already present ===================="
pactl list sinks 2>&1 | grep -i "x-maemo" || echo "  (none on sinks)"
pactl list cards 2>&1 | grep -i "x-maemo" || echo "  (none on cards)"

echo
echo "==================== 13. bluetooth ===================="
echo "-- bluez adapters/devices --"
if command -v bluetoothctl >/dev/null 2>&1; then
    timeout 10 bluetoothctl list 2>&1 | sed 's/^/  /'
    timeout 10 bluetoothctl devices 2>&1 | sed 's/^/  /'
    timeout 10 bluetoothctl info 2>&1 | sed 's/^/  /'
else
    echo "  bluetoothctl not installed"
fi
echo "-- bluez audio nodes --"
pactl list cards 2>&1 | grep -iE "bluez|Name:|Profile|Active" | sed 's/^/  /'
echo "-- ofono modems --"
if command -v ofonoctl >/dev/null 2>&1; then
    timeout 10 ofonoctl 2>&1 | head -20 | sed 's/^/  /'
else
    echo "  ofonoctl not installed (ok)"
fi

echo
echo "==================== 14. HDMI ===================="
ls -d /sys/class/drm/card*-* 2>/dev/null | while read d; do
    echo "  $(basename "$d"): status=$(cat "$d/status" 2>/dev/null)"
done
pactl list sinks 2>&1 | grep -iE "hdmi|HDMI" | sed 's/^/  /' || echo "  (no HDMI sink)"
for c in $(seq 0 7); do
    [ -e "/proc/asound/card$c" ] || continue
    amixer -c "$c" contents 2>&1 | grep -iE -B1 "hdmi" | grep "name=" | sed "s/^/  card$c: /"
done

echo
echo "==================== 15. running stack ===================="
ps -eo pid,ni,rtprio,comm 2>/dev/null | grep -E "pipewire|wireplumber|pulse" | grep -v grep | sed 's/^/  /'

echo
echo "==================== 16. existing drop-ins ===================="
for d in /etc/pipewire /etc/pipewire/pipewire.conf.d /etc/wireplumber \
         /etc/wireplumber/wireplumber.conf.d ~/.config/wireplumber \
         ~/.config/wireplumber/wireplumber.conf.d ~/.config/pipewire; do
    [ -d "$d" ] || continue
    echo "--- $d ---"
    ls -la "$d" 2>/dev/null | sed 's/^/  /'
    for f in "$d"/*.conf; do
        [ -f "$f" ] || continue
        echo "  >>> $f"
        sed 's/^/      /' "$f"
    done
done

echo
echo "==================== end of report ===================="
} > "$OUT" 2>&1

echo "wrote $OUT ($(wc -l < "$OUT") lines)"
