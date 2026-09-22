# maemo-statusmenu-volume — analysis of the PipeWire / upstream-PulseAudio breakage

Everything marked **measured** was verified on this machine (Leste dev VM,
`pipewire 1.4.9` + `pipewire-pulse` + `wireplumber`, `libpulse 17.0`, X11).
The probe programs are in `tests/` (see §7).

---

## 1. TL;DR

The applet was reverse-engineered against **Nokia's forked PulseAudio**
(`community-ssu/pulseaudio`), which provided things that do not exist in upstream
PulseAudio or in PipeWire:

| Fremantle component | What it gave the applet | Status today |
|---|---|---|
| `module-alsa-sink-volume` (Nokia "complex mixer") | `x-maemo.alsa_sink.mixer_tuning` / `alt_mixer_tuning` proplist properties (HW-register↔dB tables, per mixer) | **gone** — no `x-maemo.*` key exists on any sink (**measured**). Replaced by our own `x-maemo.volume.tuning` / `x-maemo.volume.tuning.incall`, published by wireplumber; see I7 |
| extended `module-stream-restore` (`pa_ext_stream_restore2_*`, `volume_is_absolute`, `x-maemo-stream-restore.table`, `x-maemo-match.table`, route-aware restore) | role volumes (`x-maemo`, `phone`, `animation`) that were *applied to the hardware path* and persisted | **gone** — upstream/pipewire only have v1 `pa_ext_stream_restore_*` and only restore *per-stream* volumes |
| `x-maemo.mode` / call routing inside PA | separate call (earpiece) path with its own dB table | **gone** |

The current code was patched to survive upstream PulseAudio (v1 stream-restore API,
`pa_context_set_sink_volume_by_name()`), but it still assumes all of the above.
The result:

* **in-call volume control does nothing at all** (nothing is ever sent to the server),
* the applet **never follows a default-sink change** (Bluetooth, headset, USB, call routing),
* the "normal"/"call" volumes are read from stream-restore entries that **do not exist**,
* the volume range is **hardcoded** and does not match the device,
* several latent **crashes** turn a config error into a dead `hildon-status-menu`.

---

## 2. Measured facts

| # | Fact | How it was measured |
|---|---|---|
| M1 | Sink names are `alsa_output.pci-0000_00_05.0.analog-stereo` style. The old UCM-style name (`alsa_output.0.HiFi__hw_Audio_0__sink`) returns `PA_ERR_NOENT` ("No such entity") | `pactl list sinks`, `tests/probe-pulse.c` |
| M2 | A sink proplist contains **no** `x-maemo.*` keys at all | `pa_proplist_iterate()` over the sink |
| M3 | `pa_proplist_gets(proplist, NULL)` **aborts**: `Assertion 'key' failed at .../proplist.c:279` | `tests/probe-pulse.c` |
| M4 | `pa_context_set_sink_volume_by_name()` with `cv.channels == 0` returns `NULL` / *"Invalid argument"* | `tests/probe-pulse.c` |
| M5 | `pa_ext_stream_restore_test` → v1 available. `pa_ext_stream_restore_read` returns only `sink-input-by-media-role:event` on a fresh system — **no `x-maemo`, no `phone`** | `tests/probe-pulse.c` |
| M6 | `pa_ext_stream_restore_write("sink-input-by-media-role:x-maemo", save=TRUE)` **works** under PipeWire; it maps to the PipeWire metadata key `restore.stream.Output/Audio.media.role:x-maemo` and is persisted by wireplumber in `~/.local/state/wireplumber/stream-properties` | `tests/probe-pulse.c` + state file |
| M7 | **Device** volume is persisted by wireplumber in `~/.local/state/wireplumber/default-routes` (`…:output:<port> {"channelVolumes":[…]}`) — i.e. normal-volume persistence already works if you set the *sink* volume | state file before/after `pactl set-sink-volume` |
| M8 | Subscription constants: `FACILITY_MASK=0x000F`, `TYPE_MASK=0x0030`, `CHANGE=0x0010`, `NEW=0x0000`, `REMOVE=0x0020`. The applet's `t == PA_SUBSCRIPTION_EVENT_CHANGE` matches **only** `SINK\|CHANGE`; `t == PA_SUBSCRIPTION_EVENT_SINK` matches **only** `SINK\|NEW`. `SINK\|REMOVE` (0x0020) and **all** `SINK_INPUT` events are missed | `tests/probe-events.c` |
| M9 | Running the real plugin in a harness: normal volume read + live update works (30 %→0.3158, 20 %→0.2105, 80 %→0.8421) | `tests/host-plugin.c` |
| M10 | **Default-sink change is not followed**: after `pactl set-default-sink bt_earpiece` the slider stayed at the old sink's value (0.2632) | `tests/host-plugin.c` |
| M11 | Mute is not tracked: `pactl set-sink-mute 1` changed neither icon nor slider. **Reclassified as by-design** - see I11 | `tests/host-plugin.c` |
| M12 | `parse_tuning_property(NULL, …)` returns "unchanged" (`g_quark_from_string(NULL) == 0`), so the hardcoded fallback tables stay in use — no crash, but no device tuning either | `tests/unit-math.c` (built with ASan) |
| M13 | `create_volume_steps()` produces normal = `0 … 62259` (20 steps) and incall = `32768 … 62259` (10 steps) — the top of the range is **62259, not 65536** | `tests/unit-math.c` |
| M14 | Because the slider is `step_index / 19` and the top step is 62259, the slider is scaled ≈**1.053×** vs `pactl`'s percentage and the top ~5 % of the device range is unreachable | M9 + M13 |
| M15 | `pa_vol_to_slider()`'s `steps[i+1]` read is *not* actually out of bounds (the early `vol >= steps[n-1]` return prevents it) — the `if (i == steps_size) i--;` branch is dead, not buggy | ASan run |

---

## 3. Issue list

### Blocking / functional

**I1 — In-call volume control is a no-op.** `set_volume()` (`src/item.c:1187`) in
call mode only does `priv->call_volume = volume; priv->call_volume_set = TRUE;`.
Nothing is ever sent to the server. In Fremantle the extended `module-stream-restore`
consumed the `phone` role entry and drove the earpiece mixer; without it the slider
moves and the audio does not. `hscale_value_changed_cb` and the HW-key path both end
up here, so **the slider and the volume keys are both dead during calls**.

**I2 — Default-sink changes are ignored.** `priv->normal_sink_name` is resolved once
(`context_get_server_info_cb`, `src/item.c:743`) and never re-resolved. Consequence
(**M10**): after a Bluetooth headset connects, the applet keeps reading *and writing*
the old sink name forever. Breaks: BT SCO/HFP, headset plug/unplug, USB audio, call
route changes, sink hot-unplug.

**I3 — Subscription mask decoding is wrong** (`context_subscribe_cb`, `src/item.c:670`):
```c
if ((t == PA_SUBSCRIPTION_EVENT_CHANGE) || (t == PA_SUBSCRIPTION_EVENT_SINK))
```
` t` is a *combination* of facility and type. As **M8** shows this accidentally
matches `SINK|CHANGE` and `SINK|NEW` only. `SINK|REMOVE` and every `SINK_INPUT`
event (i.e. someone else changing the call stream volume) are invisible.

**I4 — The role entries the applet reads do not exist** (**M5**).
`ext_stream_restore_read_cb` (`src/item.c:588`) looks for
`sink-input-by-media-role:x-maemo` and `…:phone`. On upstream PA / PipeWire they are
absent, so `normal_volume`/`call_volume` stay `0` until the first sink-info. Worse,
if a *stale* entry does exist it is a **second source of truth** that clobbers the
real sink volume read from `prop_sink_info_cb`.

**I5 — `normal_channels` can stay 0** (`src/item.c:1093` sets it only once, only when
the sink name matches). `set_volume()` then builds a `pa_cvolume` with
`channels == 0` and the server rejects it (**M4**). The failure is silent (one
`g_warning`), so the slider moves and nothing happens.

**I6 — NULL proplist key aborts the whole status menu** (**M3**).
`pa_proplist_gets(i->proplist, priv->incall_sink_property)` at `src/item.c:1102`
crashes if `sinks.ini` is missing or has no `[incall] sink_property`. A config
problem becomes a dead `hildon-status-menu`.

### Correctness / robustness

**I7 — Hardcoded volume range.** `create_volume_steps()` (`src/item.c:543`) hardcodes
20 steps `0…65536` and 10 steps `32768…65536`. The code ignores
`i->base_volume`, `pa_sw_volume_max_db_index()` and `PA_VOLUME_UI_MAX`. On this VM
`base_volume` is `6554` (−60 dB); other hardware uses 0 dB or +24 dB headroom.
Also the generated table tops out at 62259 (**M13**), making the slider ≈1.053× off
and the top of the range unreachable (**M14**).

**I8 — `num_steps == 1` → `g_assert` abort.** `slider_to_pa_vol()`/`pa_vol_to_slider()`
assert `num_steps > 1`, but `parse_tuning_property()` can legitimately return
`num_steps == 1` (a property with no parsable pairs). `slider_volume_decrease_step()`
checks `num_steps == 1` **after** calling `slider_to_pa_vol()` (`src/item.c:904ff`),
so the guard is useless; `slider_volume_increase_step()` checks before — asymmetric.

**I9 — `priv->pa_operation` is never assigned.** It is only ever `unref`'d
(`src/item.c:472`, `612`), so `is_running()` (`src/item.c:569`) always returns
`FALSE` and the "don't clobber an in-flight operation" guard is dead code.

**I10 — `default_sink_name` is dead** (`src/item.c:63`): never read. The code
overloads `normal_sink_name` instead, losing the distinction between
"configured sink" and "current default sink" — which is exactly the distinction
needed to fix I2.

**I11 - Mute is never read or shown** (**M11**) - **not a bug, dropped.**
Verified against a real N900: the Fremantle volume applet has no mute control.
"Muted" is expressed as volume 0, which is exactly what `get_icon_name()` does
(`volume == 0` -> `*_volume_mute`). The applet's slider is a plain `GtkHScale`
(`priv->hscale`), not `HildonVolumebar`, so there is no mute affordance to keep
in sync. Silence-on-silent-profile was the *profile* applet's job, not this one.

Nothing in the Leste stack mutes the sink either: the only mute calls are ofono's
modem-side call mute (`ofono_call_volume_set_muted`, which never touches
PulseAudio) and `hildon_volumebar_set_mute` in libhildon, which this applet
does not use. Adding mute tracking would invent UI Fremantle never had.

Residual, deliberately not handled: if an external tool mutes the sink, the
slider still reads as audible and `set_sink_volume_by_name()` does not unmute.
Accepted, since nothing in normal operation does this.

**I12 — `hildon_get_dnd()` returns TRUE whenever the `_HILDON_DO_NOT_DISTURB`
property *exists*, ignoring its value** (`src/item.c:183`). An app that sets it to
`0` still suppresses the volume banner.

**I13 — X key grab fragility.** `X_KEYCODE_UP/DOWN` are
`XKeysymToKeycode(GDK_DISPLAY(), …)` evaluated at every use. If the X keymap has no
`XF86XK_Audio*` keysyms (common when the key only exists on evdev), both are `0`:
`XGrabKey(0, …)` is meaningless and, worse, `priv->mm_key == X_KEYCODE_UP` becomes
`0 == 0`, so **every** volume key press is treated as volume-up.

**I14 — Hardcoded banner geometry** in `expose_event_cb()` (`376`, `322`, `472`,
`53/52/29/20` px) — wrong on any resolution/DPI other than 800×480.

**I15 — Dispose-path crash if D-Bus failed**: `dbus_connection_remove_filter(
dbus_g_connection_get_connection(priv->dbus), …)` with `priv->dbus == NULL`.

**I16 — `pa_glib_mainloop` is never freed** (leak), and
`g_signal_handler_disconnect(gdk_screen_get_default(), …)` disconnects from the
*current* default screen rather than the screen object stored at init.

### Design / packaging

**I17 — GConf is a build and runtime dependency but is never used** (only
`#include <gconf/gconf-client.h>`). `AM_GCONF_SOURCE_2`, `gconf-2.0` in
`PKG_CHECK_MODULES` and `-lgconf-2` can all go.

**I18 — `sinks.ini` ships device-ish config inside the applet** while Leste ships
per-device config in `leste-config` / `maemo-audio`. The `[incall] sink_property`
key is permanently dead under PipeWire (**M2**).

**I19 — Nothing tags call audio with a role.** `grep` over `voicecall`, `sphone`,
`conversations` finds no `media.role`/`media.category` usage. Even a perfectly
implemented applet has nothing to control during a call until the calls stack sets
`media.role=phone` (or `media.category=call`).

---

## 4. Proposals

### Phase 0 — stop the bleeding (small, safe, works on both PA and PipeWire)

1. **Decode subscriptions properly** (`context_subscribe_cb`):
   ```c
   pa_subscription_event_type_t fac = t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
   pa_subscription_event_type_t typ = t & PA_SUBSCRIPTION_EVENT_TYPE_MASK;

   if (fac == PA_SUBSCRIPTION_EVENT_SINK) {
       if (typ == PA_SUBSCRIPTION_EVENT_REMOVE)
           resolve_sink(menu_item);          /* re-read server info */
       else
           refresh_sink(menu_item);
   } else if (fac == PA_SUBSCRIPTION_EVENT_SERVER) {
       resolve_sink(menu_item);             /* default sink changed */
   } else if (fac == PA_SUBSCRIPTION_EVENT_SINK_INPUT) {
       refresh_call_volume(menu_item);      /* enumerate phone-role inputs */
   }
   ```
2. **Guard the proplist lookup**:
   ```c
   prop_incall = priv->incall_sink_property
                 ? pa_proplist_gets(i->proplist, priv->incall_sink_property)
                 : NULL;
   ```
3. **Never send a 0-channel cvolume** (`set_volume`):
   ```c
   if (priv->normal_channels == 0) { refresh_sink(menu_item); return; }
   pa_cvolume cv;
   pa_cvolume_set(&cv, priv->normal_channels, volume);
   ```
4. **Make `parse_tuning_property()` never return `num_steps < 2`** (fall back to the
   built-in table) and move the `num_steps == 1` check *before* the call in
   `slider_volume_decrease_step()`.
5. **Track the default sink properly**: keep `configured_sink_name` (optional, from
   config) and `current_sink_name` (from `pa_context_get_server_info()`), and
   re-resolve on `SINK|REMOVE` and on `SERVER` change. Re-validate the configured
   name once; if it disappears, fall back to the default and keep following it.
6. **Use `pa_cvolume_avg()` consistently** instead of `i->volume.values[0]`
   (the restore path already uses `avg`). Mute is deliberately *not* exposed -
   see I11.
7. **Fix the dispose path**: `if (priv->dbus) dbus_connection_remove_filter(...)`,
   `pa_glib_mainloop_free()`, disconnect the screen handler from the stored screen.
8. **Remove or fix dead state**: `pa_operation` (I9), `default_sink_name` (I10),
   `normal_volume_set`, `call_volume_set`, `slider_changed`.
9. **Drop the GConf dependency** from `configure.ac` / `Makefile.am` / `#include`.
10. **Derive banner geometry** from the widget allocation + `hildon_get_icon_pixel_size()`
    instead of hardcoded pixels.

### Phase 1 — device-driven volume range (kill the hardcoded numbers)

Replace `create_volume_steps()` with a table derived at runtime from the sink:

* `min` = mute (0) or a configurable dB floor,
* `max` = `MIN(base_volume * 2, pa_sw_volume_from_dB(cfg_max_db), PA_VOLUME_UI_MAX)`
  — or simply `base_volume` for a 0…0 dB slider,
* N steps **evenly spaced in dB**, not in raw index space. This also removes the
  ≈1.053× slider offset vs `pactl` (**M14**) and makes the slider perceptually
  correct.

Keep an **optional** per-device override shipped in `maemo-audio` (which already owns
the audio stack), e.g.

```ini
# /usr/share/maemo-audio/volume-profiles/<machine>.conf
[normal] min_db=-60  max_db=0   steps=20
[call]   min_db=-28  max_db=-15 steps=10
```
with a fallback chain *device → family → generic*, and sane defaults so a missing
file is never a bug.

**Update (implemented):** the property model was in fact revived, under new keys
`x-maemo.volume.tuning` and `x-maemo.volume.tuning.incall`, published per
device by a wireplumber `monitor.alsa.rules` drop-in shipped in `maemo-audio`.
The format is deliberately **not** Nokia's `alsa_value:dB` - the leading ALSA
control value was only ever read by `module-alsa-sink-volume`, which does not
exist on Leste, and under UCM the audio stack programs the hardware itself. The
new format is a plain ascending list of centibel values. The old keys are not
reused with a new meaning, which would silently mislead anyone porting N900
config.

Note on units: `pa_sw_volume_from_dB()` is PulseAudio's **cubic perceptual**
mapping (`dB = 60·log10(v/NORM)`), not the electrical `20·log10`. So -60 dB
is 0.1× linear, not 0.001×. The N900 tables were consumed through the same
function, so their centibel values carry over unchanged - but hand-computing
expected slider positions with the electrical formula gives wrong answers.

### Phase 2 — fix the restore model (the "no stream-restore-nemo" problem)

Pick **one** source of truth per mode:

**Normal (media/system) volume = the default sink's volume.**
* Read: `pa_context_get_sink_info_by_name(current_sink)` at start + on every change.
* Persist: let `module-device-restore` (PA) / wireplumber `default-routes`
  (PipeWire) do it — already verified working (**M7**).
* **Stop** reading `sink-input-by-media-role:x-maemo` into `normal_volume`
  (I4). If you want the role entry as a secondary persistence, it must be written on
  *every* change and read only when the sink volume is unavailable — never mixed.

**Call volume = the volume of the call stream(s)** (sink inputs with
`media.role=phone`, or `media.category=call`):
* Read: `pa_context_get_sink_input_info_list()`, filter by role, average.
* Write live: `pa_context_set_sink_input_volume()` on each phone-role input.
* Persist: `pa_ext_stream_restore_write("sink-input-by-media-role:phone", save=TRUE)`
  so new call streams inherit it — **verified to work under PipeWire (M6)**.
* When the call is being set up and no phone stream exists yet: keep the value
  locally, write the restore entry, and apply it on the first `SINK_INPUT|NEW` with
  role `phone`.
* **Prerequisite (I19)**: the calls stack must tag its streams with
  `media.role=phone`. Without that there is nothing to control.

### Phase 3 — architecture: move policy out of the applet (recommended end state)

The applet currently mixes X key grabbing, MCE D-Bus parsing, PulseAudio policy,
banner rendering and orientation quirks. Propose a small daemon
(**`maemo-audio-manager`**, or a module inside an existing maemo system service)
exposing a D-Bus API:

```
com.nokia.volume  /com/nokia/volume
  GetVolume(mode)        -> { value, min_db, max_db, steps }   // no mute: I11
  SetVolume(mode, value)
  StepVolume(mode, +1|-1)
  GetDevices()         -> [ { name, description, active, ports } ]
  signals: VolumeChanged(mode, value), DevicesChanged, CallModeChanged(bool)
```

* The applet becomes a thin UI: subscribe to signals, draw the slider/banner.
* The daemon owns: default-sink tracking, BT SCO/HFP, call routing, per-device
  persistence, hardware key events (**no X `AnyModifier` grab**, no keysym→keycode
  fragility — I13 disappears), and the per-device tuning tables.
* Other UIs (control panel, lock screen, keyboard-only devices, accessibility)
  reuse the same API.
* Testable without X.

### Should we use the native PipeWire API?

* **Pros**: direct access to node/port/device state; `default.volume` /
  `default.route` metadata; BT SCO as a real node; no dependence on the fidelity of
  pipewire-pulse's emulation (which today offers `ext-stream-restore` **v1 only** —
  no `volume_is_absolute`, no `restore_id`, no route-aware restore).
* **Cons**: loses compatibility with any remaining PulseAudio-only device; it is a
  real rewrite; you end up maintaining two backends unless PulseAudio support is
  dropped outright.
* **Recommendation**: **do not** rewrite the applet against `libpipewire` now.
  `libpulse` *is* the client library PipeWire uses, and every operation we need was
  verified to work through it (**M6**, **M7**, **M9**). Do Phase 0–2 on `libpulse`,
  and put the native PipeWire implementation behind the daemon's backend interface
  in Phase 3. Then "PW-native" is additive and the applet never needs to know.

### Suggested order

| Phase | Effort | Outcome |
|---|---|---|
| 0 | ~1 day | no crashes, device switching works |
| 1 | 1–2 days | runtime-derived range + optional config tables, no hardcoded numbers |
| 2 | 2–3 days (needs the calls stack to set `media.role=phone`) | real in-call volume + persistence |
| 3 | 1–2 weeks | daemon + thin applet; do it together with the call-routing work |

---

## 4b. Device measurements - droid4 (cpcap)

Measured on a droid4 running the same leste stack as the dev VM (pipewire 1.4.9,
wireplumber 0.5.12, pulseaudio 17.0). Card is **cpcap** via
`snd_soc_audio_graph_card2` ("Mapphone Audio"), *not* twl6040.

**Topology**

| card | device.name | role |
|---|---|---|
| 0 | `alsa_card.platform-omap-hdmi-audio.3.auto` | HDMI (own card, own `HiFi` profile) |
| 1 | `alsa_card.platform-soundcard` | cpcap (internal speaker / jack / earpiece) |

**Node naming encodes verb + port**: `alsa_output.platform-soundcard.<VERB>__<PORT>__sink`
e.g. `HiFi__Speaker`, `HiFi__Headphones`, `Voice_Call__Earpiece` (the space in
"Voice Call" becomes `_`). This is the natural match key for per-device rules.

**Jack insertion changes the UCM profile, not just the port.** `HiFi (Mic, Speaker)`
-> `HiFi (Headphones, Headset)` destroys and recreates the sink node (observed
Sink #50 -> #1217). The default sink follows automatically. This is why the
subscription fix (I2/I3) is load-bearing: without re-resolving, the applet keeps
poking a node name that no longer exists.

**UCM verbs on the cpcap card**: `HiFi` in four mic/port combinations (priority
8200-8500) and `Voice Call` in three (Headphones / Speaker / Earpiece, priority
4200). So the call path is a separate *verb*, i.e. a separate sink node.

**cpcap gain controls (TLV dB is exposed)**

| control | range | dB |
|---|---|---|
| `HiFi Playback Volume` | 0-15 | -33 .. +12, 3 dB/step |
| `Voice Playback Volume` | 0-15 | -33 .. +12, 3 dB/step |
| `Ext Playback Volume` | 0-15 | -33 .. +12, 3 dB/step |
| `Call Volume` | 0-7 | 0 .. +7, 1 dB/step |

**base_volume is the hardware 0 dB anchor** (measured by driving the sink and
reading the control):

| node | `base_volume` | PA cubic dB | control value | hardware dB |
|---|---|---|---|---|
| `HiFi__Speaker` | 41350 | -12.00 | 11 | **0.00 dB** |
| `Voice_Call__Earpiece` | 50097 | -7.00 | 9 | -6.00 dB |

So `PA_VOLUME_NORM` (65536) on the HiFi speaker is **+12 dB of boost** above
nominal - the compiled-in default ladder tops there, which is why a d4-specific
ladder is needed rather than relying on the default.

**`n_volume_steps` is 0 on both nodes.** PipeWire exposes continuous volume to
pulse clients and hides the ALSA quantisation, so the hardware step count is
*not* available through libpulse. A table derived purely from the pulse API is
therefore impossible; the per-device ladder must be declared by the audio stack
(the wireplumber `monitor.alsa.rules` drop-in in `maemo-audio`).

**Design consequence - keep absolute centibel.** The ladder values stay absolute
on PA's cubic scale rather than relative to `base_volume`. This is what makes
cross-device parity expressible: the same centibel value is the same PulseAudio
volume on every device. In particular **the in-call minimum must match the N900
at `-2000` (-20 dB = PA volume 30419)** so the quietest call level feels the
same on both devices. Under a base-relative scheme the same number would land on
a different absolute level per device and the constraint could not be met.

**Open when authoring the d4 in-call ladder**: the required span is -2000 (N900
parity floor) up to the device's call ceiling, but `base_volume` sits at -700 and
the hardware steps are 3 dB, so 10 evenly spaced steps do not land exactly on
hardware levels. Needs a listening pass on the device to pick the top and confirm
the quantisation is acceptable.

---

## 5. What is *not* broken

To avoid chasing ghosts: the **normal (non-call) volume path works today** under
PipeWire — reading the sink volume, live updates, and persistence via wireplumber
(**M7**, **M9**). The visible breakage is concentrated in calls, device switching,
mute, the range mapping, and the crash-prone edge cases.

---

## 6. Open questions for the team

1. Which component will tag call audio with `media.role=phone` (or
   `media.category=call`)? Without it Phase 2 cannot work (**I19**).
2. Should "normal" volume be **per-device** (per sink, as `module-device-restore`
   does) or **global/role-based** (as Fremantle effectively was)?
3. Do we keep the `x-maemo` role under PipeWire, or map everything to standard
   roles (`music`, `phone`, `notification`, `conversation`)?
4. Is a `maemo-audio-manager` daemon acceptable, or must the applet keep owning
   this?
5. Do we still need to support PulseAudio-only devices, or is PipeWire mandatory
   now? (This decides whether Phase 3 needs two backends.)

---

## 7. Reproducing the measurements

```
tests/probe-pulse.c    # sink names, proplist, ext-stream-restore/device-restore,
                      # NULL-name and 0-channel failure modes
tests/probe-events.c  # subscription facility/type bitmask behaviour
tests/unit-math.c     # ASan unit test of create_volume_steps / slider math /
                      # parse_tuning_property (includes src/item.c)
tests/host-plugin.c   # loads the real plugin via libhildondesktop in a window and
                      # reports the slider value over time
tests/run.sh          # drives all of the above
```
