#!/usr/bin/env python3
"""Generate a Fremantle tuning ladder for x-maemo.volume.tuning[.incall].

Model (verified against droid4 measurements, see ANALYSIS.md 4b):

    PipeWire maps slider-max (PA_VOLUME_NORM, 0 dB) onto the declared
    control's TLV *maximum*, therefore

        base_volume = pa_sw_volume_from_dB(-tlv_max)
        PA_dB       = hardware_dB - tlv_max

    so a ladder entry is  cB = (hardware_dB - tlv_max) * 100

  Cross-check:
    HiFi Playback Volume  tlv_max=+12 -> base 41350 (-12.00 dB)  measured OK
    Call Volume           tlv_max= +7 -> base 50097 ( -7.00 dB)   measured OK

Volume conversion is PulseAudio's CUBIC perceptual curve (60*log10(v/NORM)),
NOT the electrical 20*log10. Do not hand-compute with 20*log10.

Usage
-----
  # from alsamixer control values (what you read while testing)
  ./gen-tuning-table.py --ctrl-min 4 --ctrl-max 15

  # or straight from hardware dB
  ./gen-tuning-table.py --db-min -20 --db-max 12

  # N900 reference (tlv_max = 0, so cB == hardware dB)
  ./gen-tuning-table.py --db-min -20 --db-max 0 --tlv-max 0

Options
-------
  --steps N      logical steps, default 10 (Fremantle in-call)
  --tlv-max dB  declared control's max dB, default +12 (cpcap Voice Playback)
  --round 1     round cB to whole centibel (default) or coarser
"""

import argparse
import sys

PA_NORM = 65536.0


def sw_volume_from_db(db: float) -> float:
    """PulseAudio's cubic perceptual mapping."""
    return PA_NORM * (10.0 ** (db / 60.0))


def cpcap_db(ctrl: float, tlv_min=-33.0, step=3.0) -> float:
    """cpcap gain controls: ctrl 0 = -33 dB, 3 dB per step."""
    return tlv_min + step * ctrl


def main() -> int:
    ap = argparse.ArgumentParser(description=__file__)
    ap.add_argument("--ctrl-min", type=float, help="lowest alsamixer value that sounds fine")
    ap.add_argument("--ctrl-max", type=float, help="highest alsamixer value that sounds fine")
    ap.add_argument("--db-min", type=float, help="lowest hardware dB that sounds fine")
    ap.add_argument("--db-max", type=float, help="highest hardware dB that sounds fine")
    ap.add_argument("--steps", type=int, default=10, help="logical steps (default 10)")
    ap.add_argument("--tlv-max", type=float, default=12.0,
                   help="declared control TLV max in dB (default +12)")
    ap.add_argument("--cpcap", action="store_true", default=True,
                   help="interpret --ctrl-* with the cpcap -33dB/3dB-per-step curve (default)")
    args = ap.parse_args()

    if args.ctrl_min is not None or args.ctrl_max is not None:
        if args.ctrl_min is None or args.ctrl_max is None:
            ap.error("give both --ctrl-min and --ctrl-max, or use --db-min/--db-max")
        db_min = cpcap_db(args.ctrl_min)
        db_max = cpcap_db(args.ctrl_max)
        src = f"control values {args.ctrl_min:g}..{args.ctrl_max:g}"
    elif args.db_min is not None and args.db_max is not None:
        db_min, db_max = args.db_min, args.db_max
        src = "hardware dB"
    else:
        ap.error("need --ctrl-min/--ctrl-max or --db-min/--db-max")

    if db_max <= db_min:
        ap.error("max must exceed min")
    if args.steps < 2:
        ap.error("--steps must be >= 2")

    # even spacing in hardware dB -> centibel on the PA scale
    span = db_max - db_min
    step_db = span / (args.steps - 1)
    entries = []
    for i in range(args.steps):
        hw = db_min + step_db * i
        cb = round((hw - args.tlv_max) * 100.0)
        entries.append(cb)

    # strictly ascending check (the C parser rejects non-ascending tables)
    for a, b in zip(entries, entries[1:]):
        if b <= a:
            print(f"error: table not ascending ({a} -> {b})", file=sys.stderr)
            return 1

    table = ",".join(str(v) for v in entries)

    print(f"# source:        {src}")
    print(f"# hardware dB:   {db_min:g} .. {db_max:g}  ({step_db:.2f} dB per step)")
    print(f"# tlv_max:       {args.tlv_max:+g} dB  ->  base_volume "
          f"{round(sw_volume_from_db(-args.tlv_max))} "
          f"({-args.tlv_max:+.2f} dB PA)")
    print(f"# steps:         {args.steps}")
    print()
    print(table)
    print()

    # per-entry detail so it can be checked against the device
    print("#  idx   cB      PA vol    %NORM   hardware dB   (ctrl)")
    for i, cb in enumerate(entries, 1):
        v = sw_volume_from_db(cb / 100.0)
        hw = cb / 100.0 + args.tlv_max
        ctrl = (hw + 33.0) / 3.0
        print(f"#  {i:>3} {cb:>6}  {round(v):>7}  {v / PA_NORM * 100:5.1f}%   "
              f"{hw:>7.2f}      ({ctrl:.2f})")

    # sanity: where does the table sit relative to base_volume?
    base = sw_volume_from_db(-args.tlv_max)
    below = sum(1 for e in entries if sw_volume_from_db(e / 100.0) < base)
    print()
    print(f"# entries below base_volume (hardware < 0 dB): {below}/{args.steps}")
    print(f"# entries above base_volume (boost):           {args.steps - below}/{args.steps}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
