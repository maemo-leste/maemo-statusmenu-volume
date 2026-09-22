#!/bin/sh
# d4-callvol.sh - find out what actually governs in-call loudness on droid4.
#
# Run this DURING a call (over SSH or a serial console - the call UI owns the
# screen). It answers three separate questions that the applet design hinges on:
#
#   A. Which control changes loudness when written directly (no PipeWire)?
#   B. Does the PipeWire sink volume drive that control at all?
#   C. Is any audio actually streaming through the host sink during a call?
#
# It saves every value it touches and restores them on exit (Ctrl-C is caught).
#
# Usage:  ssh user@d4
#         (start/answer a call with audible far-end audio)
#         sh d4-callvol.sh
#         ...send me /tmp/d4-callvol-*.log...
#
# Env: CARD=1  ALSA card with the cpcap/modem controls
#      SINK=    override the voice-call sink name

set -u

CARD=${CARD:-1}
LOG=/tmp/d4-callvol-$(date +%H%M%S).log
SAVED=/tmp/d4-callvol-saved.$$
: > "$LOG"

say() { printf '%s\n' "$*" | tee -a "$LOG"; }
ts()  { date +%H:%M:%S; }

CTL_MAX() { amixer -c "$CARD" cget name="$1" 2>/dev/null \
              | sed -n 's/.*max=\([0-9]*\).*/\1/p' | head -1; }
CTL_VAL() { amixer -c "$CARD" cget name="$1" 2>/dev/null \
              | sed -n 's/.*: values=\([0-9]*\).*/\1/p' | head -1; }
CTL_DB()  { amixer -c "$CARD" cget name="$1" 2>/dev/null \
              | sed -n 's/.*dBscale-min=\([^,]*\),step=\([^,]*\).*/min \1 step \2/p' | head -1; }

CTLS="Call Volume Voice Playback Volume HiFi Playback Volume Ext Playback Volume"

# ---------------------------------------------------------------- save/restore
save_state() {
  : > "$SAVED"
  for c in $CTLS; do
    v=$(CTL_VAL "$c")
    [ -n "$v" ] && printf '%s\t%s\n' "$c" "$v" >> "$SAVED"
  done
}

restore_state() {
  say ""
  say "=== $(ts) restoring saved control values ==="
  while IFS='	' read -r c v; do
    say "  $c -> $v"
    amixer -c "$CARD" cset name="$c" "$v" >/dev/null 2>&1
  done < "$SAVED"
  rm -f "$SAVED"
  say "log kept at $LOG"
  exit 0
}
trap restore_state INT TERM

# ---------------------------------------------------------------- helpers
snap() {
  say "--- $(ts) control snapshot ---"
  for c in $CTLS; do
    v=$(CTL_VAL "$c")
    [ -n "$v" ] && say "  $(printf '%-22s' "$c") = $v   ($(CTL_DB "$c"))"
  done
}

# sweep a control across values, pausing so you can listen
sweep() {
  c=$1; shift
  m=$(CTL_MAX "$c")
  if [ -z "$m" ]; then say "  (no such control: $c)"; return; fi
  say "--- sweeping '$c' (max=$m) : listen for loudness changes ---"
  for v in "$@"; do
    [ "$v" -le "$m" ] || v=$m
    amixer -c "$CARD" cset name="$c" "$v" >/dev/null 2>&1
    say "  $(ts)  $c = $v   <-- listen 3s"
    sleep 3
  done
}

# ---------------------------------------------------------------- 0. context
say "=== droid4 in-call volume probe ==="
say "started $(ts)"
say ""
say "NOTE: run this with audible far-end audio (a person talking, not silence)."
say "Press ENTER to begin (Ctrl-C at any time restores everything)."
read _

save_state
snap

say ""
say "=== sinks visible right now ==="
pactl list sinks short 2>/dev/null | tee -a "$LOG"

VOICE_SINK=${SINK:-$(pactl list sinks short 2>/dev/null \
                    | awk '/Voice_Call|VoiceCall/ {print $2; exit}')}
say ""
say "voice-call sink: ${VOICE_SINK:-<NONE FOUND>}"
if [ -z "$VOICE_SINK" ]; then
  say "  No Voice_Call sink -> the VoiceCall UCM verb is not active."
  say "  Make/answer a call so the profile switches, then re-run."
  exit 1
fi

# ---------------------------------------------------------------- C. streaming?
say ""
say "=== C. is anything streaming into the voice sink? ==="
say "-- sink-inputs --"
pactl list sink-inputs 2>/dev/null \
  | sed -n 's/^\t\(Sink Input\|Media Name\|Media Role\|Sink\|State\) : /  \1 : /p' \
  | tee -a "$LOG"
say "-- sample-rate / buffered time of the voice sink --"
pactl list sink 2>/dev/null | awk -v s="$VOICE_SINK" '
  /^Sink #/ {p=0} $0 ~ s {p=1}
  p && /^\t(State|Sample Specification|Base Volume|Volume|Monitor Source)/ {print "  " $0}
' | tee -a "$LOG"
say "  (if the sink State is IDLE and no input is attached, the host is not"
say "   in the sample path - consistent with a DAI<->DAI call route)"

# ---------------------------------------------------------------- A. direct
say ""
say "=== A. direct control writes, PipeWire NOT involved ==="
say ">>> Which of these changes what YOU HEAR? <<<"
sweep "Call Volume" 0 2 4 7
sweep "Voice Playback Volume" 0 4 9 12 15

# ---------------------------------------------------------------- B. via PW
say ""
say "=== B. via PipeWire sink volume (what the applet would do) ==="
say ">>> Does loudness track this, and which control moves? <<<"
for v in 8192 16384 32768 50097 65536 81920; do
  pactl set-sink-volume "$VOICE_SINK" "$v" 2>/dev/null
  say "  $(ts)  sink=$v  $(printf '%-20s' 'Call Volume'  )=$(CTL_VAL 'Call Volume')  $(printf '%-22s' 'Voice Playback Volume')=$(CTL_VAL 'Voice Playback Volume')   <-- listen 3s"
  sleep 3
done

snap
say ""
say "=== done ==="
say "Tell me, for each of A and B: did loudness change, and which control moved?"
restore_state
