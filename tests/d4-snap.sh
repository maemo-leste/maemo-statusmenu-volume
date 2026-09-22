#!/bin/sh
# Compact audio state snapshot for droid4 (maemo leste).
#
# Run this BEFORE and AFTER each event and send me the pair; I diff them.
# The point is to see exactly what changes in the PipeWire graph, the
# default sink, the ports and the applet-visible properties when the
# output device changes - which decides whether the wireplumber rules
# match on node name or on port.
#
#   sh d4-snap.sh before-jack
#   ... plug headphones ...
#   sh d4-snap.sh after-jack
#
# Writes /tmp/d4-snap-<label>.txt. Read-only apart from explicit
# 'pactl set-*' lines you run yourself between snapshots.

LABEL="${1:-snap}"
OUT="/tmp/d4-snap-${LABEL}.txt"

{
echo "===== snapshot: $LABEL  at $(date -Is) ====="

echo
echo "-- defaults --"
pactl info 2>&1 | grep -E "Default (Sink|Source|Card)"

echo
echo "-- cards: name / active profile --"
pactl list cards 2>&1 | grep -E "^Card|Name:|Active Profile:|^	Profile" | head -60

echo
echo "-- sinks: name / state / port / volume / mute --"
pactl list sinks 2>&1 | grep -E "^Sink|Name:|State:|Active Port:|Base Volume:|Volume:|Mute:|Description:" | head -60

echo
echo "-- sources: name / state / port --"
pactl list sources 2>&1 | grep -E "^Source|Name:|State:|Active Port:|Mute:" | head -60

echo
echo "-- card profiles + ports (what UCM reports) --"
echo "(pactl rejects '-v' for cards/sinks/sources; extract the blocks directly)"
pactl list cards 2>&1 | awk '
/^Card / { print ""; print; inp=0; inport=0; next }
/^[[:space:]]+Profiles:/       { inp=1;    print; next }
/^[[:space:]]+Active Profile:/ { inp=0;    print; next }
/^[[:space:]]+Ports:/         { inport=1; print; next }
{ if (inp || inport) print }
'

echo
echo "-- node names + ports (PipeWire view) --"
if command -v pw-dump >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1; then
    pw-dump > /tmp/d4-snap-pwdump.json 2>/dev/null
    python3 - <<'PY'
import json
keys = ('node.name','node.description','node.port','media.class',
        'device.name','device.description','device.form-factor',
        'device.api','device.bus','x-maemo.volume.tuning',
        'x-maemo.volume.tuning.incall')

def parts(obj):
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
    d = json.load(open('/tmp/d4-snap-pwdump.json'))
except Exception as e:
    print("  pw-dump parse failed:", e); raise SystemExit

shown = 0
for obj in d:
    props, params = parts(obj)
    mc = props.get('media.class', '')
    if mc not in ('Audio/Sink', 'Audio/Source', 'Audio/Device'):
        continue
    shown += 1
    print(f"  [{mc}] id={obj.get('id')}")
    for k in keys:
        if k in props:
            print(f"      {k} = {props[k]}")
    for pname, v in params.items():
        if pname in ('EnumPort', 'Profile', 'Ports'):
            print(f"      params.{pname}: {json.dumps(v)[:800]}")
print(f"  ({shown} audio nodes matched)")
PY
else
    echo "  (pw-dump or python3 missing)"
fi

echo
echo "-- jack sysfs state --"
for sw in /sys/class/sound/jack/*; do
    [ -e "$sw" ] || continue
    for f in "$sw"/*; do
        [ -f "$f" ] && echo "  $(basename "$sw")/$(basename "$f") = $(cat "$f" 2>/dev/null)"
    done
done

echo
echo "-- amixer: gain / jack / switch controls with dB ranges --"
for c in $(seq 0 7); do
    [ -e "/proc/asound/card$c" ] || continue
    echo "  --- card$c ---"
    amixer -c "$c" contents 2>&1 \
      | grep -E -A3 "^numid=.*(Volume|Jack|Switch)" | sed "s/^/    /"
done

echo
echo "-- x-maemo tuning properties --"
pactl list sinks 2>&1 | grep -i "x-maemo" | sed 's/^/  /' || true
pactl list cards 2>&1 | grep -i "x-maemo" | sed 's/^/  /' || true
echo "  (nothing above = no tuning property published yet)"

echo
echo "-- active stream routing (if any) --"
pactl list sink-inputs 2>&1 | grep -E "Index:|Sink:|media.role|Application Name:|media.name" | head -30

echo
echo "===== end snapshot: $LABEL ====="
} > "$OUT" 2>&1

echo "wrote $OUT ($(wc -l < "$OUT") lines)"
