# gemifi driver for Zynthian: notes

Status: **implemented** (2026-11-03) in the zynthian-ui fork [`vantreeseba/zynthian-ui`](https://github.com/vantreeseba/zynthian-ui), branch `ui-modernize`: `zyngine/ctrldev/zynthian_ctrldev_gemifi.py` plus `zyngine/ctrldev/gemifi/` (this repo's MIT protocol, device and mapping code, a Zynthian adapter, and tests that need no hardware). Unit-tested, not yet tried on a Zynthian with a device. These notes are the Zynthian-specific half of writing a gemifi driver (a "ctrldev") in a zynthian-ui fork. The gemifi half, the same for every DAW, is [`protocol/daw-scripts.md`](../../protocol/daw-scripts.md).

The driver does in Zynthian what the Ableton Live script ([`../ableton-live/`](../ableton-live/)) and the Bitwig extension ([`../bitwig/`](../bitwig/)) do in those DAWs. It finds a gemifi device, asks it which controls it has and what each one is for, hears presses and moves, and drives the device's LEDs, knob pick-up and encoder rings from Zynthian's state.

Contents:
1. [Licensing](#1-licensing)
2. [What the driver talks to](#2-what-the-driver-talks-to)
3. [Zynthian's driver interface](#3-zynthians-driver-interface)
4. [The gemifi side](#4-the-gemifi-side)
5. [Roles → Zynthian](#5-roles--zynthian)
6. [Feedback: Zynthian state → the device](#6-feedback-zynthian-state--the-device)
7. [Testing](#7-testing)
8. [Open questions](#8-open-questions)

The gemifi protocol for scripts: [`protocol/daw-scripts.md`](../../protocol/daw-scripts.md) (normative source: [`protocol/sysex.md`](../../protocol/sysex.md)). Role names: [`hosts/README.md`](../README.md).

---

## 1. Licensing

- zynthian-ui is **GPL v3 or later**. A ctrldev driver subclasses its base classes and runs inside it, so **the driver is GPL** and lives in the Zynthian fork (or upstream), not in gemifi.
- gemifi (this repo, including this spec and the reference implementations) is **MIT**. MIT code can be copied into a GPL project, so the driver may reuse the codec in [`hosts/ableton-live/gemifi/protocol.py`](../ableton-live/gemifi/protocol.py) and the logic in [`device.py`](../ableton-live/gemifi/device.py) and [`mapping.py`](../ableton-live/gemifi/mapping.py), keeping the MIT notice for the copied parts.

## 2. What the driver talks to

**A gemifi device** is a MIDI controller (keyboard, fader box, pedal…) running gemifi firmware, built from a device YAML. Over USB it shows up as one MIDI port, **`gemifi (<friendly name>)`**, the main port. A device with Mackie Control (`mcu:` in its YAML) has a second port, `… DAW`.

- **Main port:** everything the controller plays (notes, CCs, pitch bend), plus gemifi's own SysEx protocol. **The driver uses this port.**
- **DAW port (optional):** Mackie Control only. Zynthian's own `zynthian_ctrldev_mackiecontrol` driver can be bound to it independently; the gemifi driver ignores it.

**What the driver gets from the device (host feedback, "F3"):**
- a list of **components** (buttons, the key matrix, analog inputs, encoders, LEDs, displays), each with an `id`, a display `name` and a **`role`**: a string saying what a host should connect it to (`play`, `volume_3`, `device_param_1`, …). The role comes from the YAML's `daw:` key, or the component's `mcu:` name;
- **EVENT** messages when a component is pressed or moved, addressed by component index (independent of which CC or note it sends);
- **FB_\*** messages to set LEDs, pick-up targets and encoder rings, by component index. They're never answered.

The device's normal MIDI (notes, CCs) still goes out as usual, so a keyboard keeps playing synths while the driver runs.

## 3. Zynthian's driver interface

Checked against zynthian-ui `main` on 2026-11-03: `zyngine/zynthian_ctrldev_manager.py`, `zyngine/ctrldev/zynthian_ctrldev_base.py`, `zynthian_ctrldev_launchpad_x.py` and `zynthian_ctrldev_mackiecontrol.py`. Re-check if your fork is older or newer.

### 3.1 Where the file goes, and naming
- One Python module per driver. The manager scans **`zyngine/ctrldev_user/*.py` first**, then `zyngine/ctrldev/*.py`, and imports `zyngine.<dir>.<module>`.
- The class name must equal the module name: **`zynthian_ctrldev_gemifi.py`** defining **`class zynthian_ctrldev_gemifi`**. The implementation lives in `zyngine/ctrldev/` (next to the Mackie Control driver), with its support package in `zyngine/ctrldev/gemifi/`. Dropping both into `zyngine/ctrldev_user/` instead also works, for a Zynthian that doesn't run the fork.
- `driver_name` defaults to the class name minus the `zynthian_ctrldev_` prefix (`gemifi`). Set `driver_description`, e.g. `"gemifi controllers (host feedback)"`.
- Setting `DRIVER_DEVELOPMENT = True` in the manager reloads the driver modules on every load, which is handy while writing it.

### 3.2 Base class
Subclass **`zynthian_ctrldev_zynmixer`**, from `zynthian_ctrldev_base.py`. It adds mixer access on top of `zynthian_ctrldev_base`:
- `self.state_manager`, `self.chain_manager`, `self.zynseq`, and `self.zynmixer` (= `state_manager.zynmixer_chan`);
- `self.idev` (input slot) and `self.idev_out` (output slot, 0 or None for no output);
- `set_mixer_param(param, pos, value)`, `get_mixer_param(param, pos)`, `toggle_mixer_param(param, pos)`, `nudge_mixer_param(param, pos, value, fine=False)`. `param` is one of `"level"`, `"balance"`, `"mute"`, `"solo"`, `"record"`; `pos` is the chain's display index (`-1` = main chain);
- `update_mixer_strip(chan, symbol, value, mixbus=False)`, called (queued) on every mixer change via `zynsigman.S_MIXER / SS_ZYNMIXER_SET_VALUE`;
- column scrolling: `self.cols`, `self.scroll_h`, `get_filtered_chain_by_index(i)`, `get_num_filtered_chains()`.

### 3.3 Matching a device (important)
- `dev_ids` is a list of device-ID strings. The manager **autoloads** a driver only when the port's device ID (`zynautoconnect.get_midi_in_devid(izmip)`) is **exactly** one of them (a dict lookup; no wildcards or prefixes).
- `dev_ids = ["*"]` registers the driver as available for any device, but **"*" drivers aren't autoloaded**. The user picks it for a device in Zynthian's UI; that's how the Mackie Control driver works.
- Device IDs look like `"Launchpad X IN 1"` (the ALSA port name). A gemifi device's will be derived from its USB product string `gemifi (<friendly name>)`. The exact form (e.g. a trailing ` IN 1` or ` MIDI 1`) is **to check** on a Zynthian: the manager logs `Loaded ctrldev driver '<name>' for '<dev_id>'`.
- **Recommendation:** `dev_ids = ["*"]` (pick it per device), plus an optional list of known IDs for autoload, e.g. `"gemifi (Keystation Mini 32) IN 1"` once the exact form is known. Either way, the driver **confirms** it's talking to a gemifi device with HELLO ([daw-scripts.md §3](../../protocol/daw-scripts.md#3-a-scripts-session)) and stays idle if not.

### 3.4 Routing
- `unroute_from_chains` decides whether the device's MIDI still reaches Zynthian's chains: `True` = none (the default), `False` = all, or an int bitmask of channels to unroute.
- **A gemifi driver must use `unroute_from_chains = False`**, or the keyboard stops playing synths.
- Because nothing is unrouted, a CC that a component sends may also reach a chain that MIDI-learned it. Users should give role-mapped components CCs nothing else uses, or set `daw:` roles only on components that aren't also used for MIDI learn. This is documented behaviour, not something the driver fixes.

### 3.5 Lifecycle and I/O
- `__init__(self, state_manager, idev_in, idev_out=None)`: call `super().__init__`.
- `init()`: called once after load, with the MIDI handler already active, so SysEx replies during init arrive. Register signals (`zynsigman.register_queued(...)`) and start the gemifi session (daw-scripts.md §3). Call `super().init()`.
- `end()`: unregister signals, send FB_RELEASE 0 and restore `debug.events` (daw-scripts.md §3, step 6), stop timers, then call `super().end()`.
- `midi_event(self, ev)`: every message from the device; `ev` is a bytes-like message. Channel messages have `ev[0]` = status; **SysEx arrives whole, with `ev[0] == 0xF0`** (see the Launchpad X driver). Return `True` when the driver handled it.
- `refresh()`: re-send all feedback (chains changed, a snapshot loaded…). Called by the base class on chain add, remove and move, snapshot load, and from `init()`.
- `on_active_chain(active_chain_id)`: the selected chain changed (`SS_SET_ACTIVE_CHAIN`).
- `light_off()`: all device LEDs off (used on sleep and end).
- **Sending:** `lib_zyncore.dev_send_midi_event(self.idev_out, msg, len(msg))` with `msg` a `bytes` (SysEx included). Check `self.idev_out` first.
- **UI actions:** `self.state_manager.send_cuia("NAME", params=[...])`.
- **Timers:** the base class has none. The driver needs a ~100 ms tick for request timeouts and the keep-alive (daw-scripts.md §3). A `threading.Timer` or a small daemon thread works. Do the actual sends from it, and keep its state access simple: `midi_event` and the signal callbacks run on other threads, so use a lock around the device state machine.

## 4. The gemifi side

Everything about the protocol is in **[`protocol/daw-scripts.md`](../../protocol/daw-scripts.md)**, the base for every gemifi DAW integration: framing, the session (find → enumerate → enable EVENTs → run → BOOTED → exit), every message with example frames, the behaviour rules and the role list. Implement that, with the Zynthian specifics below:

- **Port:** the driver gets the device's main port as its input/output slot pair (`self.idev`, `self.idev_out`). SysEx arrives whole in `midi_event()` (`ev[0] == 0xF0`); send with `lib_zyncore.dev_send_midi_event(self.idev_out, msg, len(msg))`.
- **Timer:** the session needs a ~100 ms tick (daw-scripts.md §7.2); Zynthian's base class has none (§3.5 above).
- **Start and exit:** run the session from `init()`, and send FB_RELEASE 0 and restore `debug.events` in `end()`.
- **Re-sending:** `refresh()` (chains changed, snapshot loaded) re-sends everything, as after enumeration.
- **Several devices:** one driver instance per device, each with its own session.

## 5. Roles → Zynthian

"Verified" means the API is used this way by an existing driver in zynthian-ui (named). "To check" means it's a suggestion that needs confirming in your fork.

| Role(s) | Component | Zynthian | Status |
|---|---|---|---|
| `volume_N` | analog / encoder | `set_mixer_param("level", pos)` with `pos = scroll_h + N − 1`; value scaled to the mixer's range | verified (mackiecontrol). Check the value range; mackiecontrol converts 14-bit to the mixer level |
| `pan_N` | analog / encoder | `"balance"`: `set_mixer_param` (analog) or `nudge_mixer_param` (encoder delta) | verified (mackiecontrol) |
| `mute_N`, `solo_N`, `arm_N` | button | `toggle_mixer_param("mute" / "solo" / "record", pos)` on press | verified (mackiecontrol) |
| `select_N` | button | make chain `pos` the active chain | to check (chain_manager API) |
| `bank_prev`, `bank_next` | button | `scroll_h ∓ 8` (clamped to `get_num_filtered_chains()`), then `refresh()` | verified pattern (base class scrolling) |
| `master_volume` | analog / encoder | `"level"` on `pos = -1` (main chain) | verified (mackiecontrol) |
| `track_volume`, `track_pan`, `track_mute`, `track_solo`, `track_arm` | as above | the same mixer params on the **active** chain (its display index) | to check (finding the active chain's index) |
| `track_prev`, `track_next` | button | select the previous / next chain (`ARROW_LEFT` / `ARROW_RIGHT` CUIA in the mixer view is one option) | to check |
| `play` | button | `TOGGLE_AUDIO_PLAY` (or `TOGGLE_MIDI_PLAY`, a driver option) | verified CUIA names (mackiecontrol) |
| `record` | button | `TOGGLE_AUDIO_RECORD` (or `TOGGLE_MIDI_RECORD`) | verified CUIA names |
| `stop` | button | `STOP_AUDIO_PLAY` + `STOP_AUDIO_RECORD` (and the MIDI ones) | verified CUIA names |
| `device_param_1..4` | encoder / analog | Zynthian's four rotary controls on the current screen: `send_cuia("ZYNPOT", params=[n − 1, delta])` | verified CUIA (mackiecontrol). It acts on whatever screen is shown, which is Zynthian's model |
| `device_param_5..8` | | no direct equivalent: ignore, or a second page via `ZYNSWITCH` | to check |
| `scene_launch`, `scene_prev`, `scene_next`, `stop_all_clips` | button | the zynpad sequencer: `self.zynseq.libseq.togglePlayState(scene, phrase, chan)`, `self.zynseq.scene` | verified API (launchpad_x); the mapping to scenes is to check |
| `loop`, `metronome`, `tap_tempo`, `undo`, `redo`, `rewind`, `fast_forward`, `overdub`, `session_record`, `automation_arm`, `tempo`, `crossfader`, `cue_volume`, `send_a`, `send_b`, `device_*` navigation | | look for matching CUIAs in your fork's `zynthian_gui` CUIA list; ignore the ones without (log once) | to check |
| keys, displays | | not mapped: the keys play the synths as normal MIDI | n/a |

**EVENT handling:** a button role acts on **press** (`value` 1) unless it's momentary by nature (e.g. `rewind` held). An analog EVENT carries 0..16383 (use the top bits as needed); an encoder EVENT carries a signed detent delta.

## 6. Feedback: Zynthian state → the device

| What changed in Zynthian | Signal / source | Send |
|---|---|---|
| a mixer strip's mute/solo/record/level/balance | `update_mixer_strip(chan, symbol, value)` (zynmixer base, `S_MIXER / SS_ZYNMIXER_SET_VALUE`) | FB_LED for buttons with `mute_N` / `solo_N` / `arm_N` (and `track_*`) on that strip; FB_VALUE (`takeover` = 1 only) for `volume_N` / `pan_N` / `master_volume` |
| the bank scrolled | the driver's own `scroll_h` | re-send everything strip-related for the new bank (FB_LEDS where consecutive) |
| audio player / recorder state | `S_AUDIO_PLAYER / SS_AUDIO_PLAYER_STATE`, `S_AUDIO_RECORDER / SS_AUDIO_RECORDER_STATE`; `state_manager.status_audio_player`, `state_manager.audio_recorder.rec_proc` | FB_LED for `play`, `record`, `stop` (verified: mackiecontrol `refresh_audio_transport`) |
| MIDI player / recorder state | `S_STATE_MAN / SS_MIDI_PLAYER_STATE`, `SS_MIDI_RECORDER_STATE`; `status_midi_player`, `status_midi_recorder` | the same, if the driver option uses MIDI transport |
| the active chain | `on_active_chain(id)` | `select_N` LEDs; everything `track_*` |
| chains added/removed/moved, snapshot loaded | `refresh()` (base class) | everything |
| sequencer pad/phrase state | zynpad (`update_seq_state` in `zynthian_ctrldev_zynpad`) | `scene_*` LEDs, if mapped |

- An encoder with a ring (`ring` > 0) mapped to a parameter gets **FB_RING**: boost/cut (mode 1) for bipolar parameters (balance), wrap (mode 2) otherwise, `value` 0..16383 = the parameter's position. Use flags bit 1 (ring off) when its role maps to nothing in the current bank.
- **Send only changes.** The device takes notifications as fast as USB carries them, but Zynthian's level meters and similar can change often. Don't map meters to LEDs.

## 7. Testing

**Without hardware:** a fake device that answers HELLO, GET_COMPONENT, SET_PARAM and GET_PARAM per daw-scripts.md §4, records FB_\* and can send EVENT and BOOTED. The Live script's [`tests/fakes.py`](../ableton-live/tests/fakes.py) is one to port. Cross-check the codec against `protocol/test-vectors.json` (`frames`, and the `feedback` section).

**With a device:**
- **Setup:** flash a gemifi device. The Keystation Mini 32 default YAML has roles via `daw:` on some buttons once you add them; an ESP32-S3 fader box with `daw: volume_N` is the most useful. Then on the Zynthian: check out the fork's `ui-modernize` branch (or copy `zynthian_ctrldev_gemifi.py` and `gemifi/` into `zyngine/ctrldev/` or `zyngine/ctrldev_user/`), restart the UI, select the "gemifi" driver for the device's port. The driver's own tests run with `python3 -m pytest zyngine/ctrldev/gemifi/tests`.
- **Checklist:**
  - [ ] the log shows the dev_id (§3.3) and "gemifi: N components";
  - [ ] keys still play the active chain (`unroute_from_chains = False`);
  - [ ] faders move chain levels; mute/solo LEDs follow the mixer, including changes made on the Zynthian's touchscreen;
  - [ ] knobs with pick-up don't jump after a level changes on screen;
  - [ ] bank next/prev shifts strips and LEDs;
  - [ ] play/record LEDs follow the audio player/recorder;
  - [ ] unplug/replug and power-cycle the device: the driver recovers (BOOTED or keep-alive) and re-sends;
  - [ ] unload the driver: LEDs go back to the device's own state (FB_RELEASE).

The `gemifi.py` host tool (`tools/host/`) helps here: `gemifi.py components` lists what the device reports, and `gemifi.py log` shows what it receives and sends.

## 8. Open questions

1. **Device-ID format** of a gemifi port in Zynthian (§3.3), and whether to ship known IDs for autoload.
2. **Active-chain API:** the cleanest way to get and set the active chain and its display index from a driver (§5, `select_N`, `track_*`).
3. **CUIA list:** which CUIAs your Zynthian version has for loop, metronome, tempo, undo/redo; the table above only uses ones seen in existing drivers.
4. **Mixer value ranges:** `"level"` and `"balance"` value ranges, and the conversion mackiecontrol uses.
5. **Thread safety:** which thread calls `midi_event` and the queued signal handlers, to size the locking (§3.5).
6. **Displays:** the Code 61 has a 6-character one (GET_COMPONENT kind 6). FB_TEXT could show the active chain's name.
