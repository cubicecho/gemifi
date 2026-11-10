# gemifi driver for Zynthian: notes

Status: **implemented** (2026-11-03) in the zynthian-ui fork [`vantreeseba/zynthian-ui`](https://github.com/vantreeseba/zynthian-ui), branch `ui-modernize`: `zyngine/ctrldev/zynthian_ctrldev_gemifi.py` plus `zyngine/ctrldev/gemifi/` (this repo's MIT protocol, device and mapping code, a Zynthian adapter, and tests that need no hardware). 2026-11-04 (#21): the clip grid on zynpad (§5.1), the nav roles, `zynthian.<cuia>`, `shift`, `strip_prev` / `strip_next`, and the `param` display; the codec reads pads (kind 7), meters (kind 8, reserved), the analog `touch` byte and EVENT types 5 and 7. 2026-11-10 (gemifi v0.37): clip pads in Zynthian's launcher colours, full colour on `rgb_serial` pads (§5.1); coalesced, change-only feedback per daw-scripts.md §5.12 (§6.1); the codec knows LOCKED, RAW, UNLOCKED, encoder mode 4 and the analog `adc_min` / `adc_max`. Unit-tested, not yet tried on a Zynthian with a device. These notes are the Zynthian-specific half of writing a gemifi driver (a "ctrldev") in a zynthian-ui fork. The gemifi half, the same for every DAW, is [`protocol/daw-scripts.md`](../../protocol/daw-scripts.md).

The driver does in Zynthian what the Ableton Live script ([`../ableton-live/`](../ableton-live/)) and the Bitwig extension ([`../bitwig/`](../bitwig/)) do in those DAWs. It finds a gemifi device, asks it which controls it has and what each one is for, hears presses and moves, and drives the device's LEDs, knob pick-up and encoder rings from Zynthian's state.

Contents:
1. [Licensing](#1-licensing)
2. [What the driver talks to](#2-what-the-driver-talks-to)
3. [Zynthian's driver interface](#3-zynthians-driver-interface)
4. [The gemifi side](#4-the-gemifi-side)
5. [Roles → Zynthian](#5-roles--zynthian) ([clip grid](#51-clip-grid), [displays](#52-displays))
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
Subclass **`zynthian_ctrldev_zynpad`** and **`zynthian_ctrldev_zynmixer`**, from `zynthian_ctrldev_base.py`, in that order (as the APC40 mk2, MPK249 and Launchkey Mini mk4 drivers do). `zynthian_ctrldev_zynpad` brings the launcher: it registers for zynseq's signals and calls `update_seq_state(phrase, chan)` on every play-state change, and its `on_pad_press` / `on_pad_release` launch a pad the way every pad controller does (§5.1). The gemifi driver overrides `update_seq_state`, `update_pad`, `pad_off` and `refresh` (it skips zynpad's `refresh`, which repaints through `light_off()` and `update_pad()`), and draws the grid itself. `zynthian_ctrldev_zynmixer` adds mixer access on top of `zynthian_ctrldev_base`:
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
- **Timer:** the session needs a ~100 ms tick (daw-scripts.md §7.2); Zynthian's base class has none (§3.5 above). The driver's session thread also flushes coalesced changes between ticks, at most every 40 ms (§6.1).
- **Start and exit:** run the session from `init()`, and send FB_RELEASE 0 and restore `debug.events` in `end()`.
- **Re-sending:** `refresh()` (chains changed, snapshot loaded) re-sends everything, as after enumeration.
- **Several devices:** one driver instance per device, each with its own session.

## 5. Roles → Zynthian

"Verified" means the API is used this way by an existing driver in zynthian-ui (named). "To check" means it's a suggestion that needs confirming in your fork.

| Role(s) | Component | Zynthian | Status |
|---|---|---|---|
| `volume_N` | analog / encoder | `set_mixer_param("level", pos)` with `pos = scroll_h + N − 1`; value scaled to the mixer's range | verified (mackiecontrol). Check the value range; mackiecontrol converts 14-bit to the mixer level |
| `pan_N` | analog / encoder | strip N's `balance` controller: set (analog) or nudged by its own nudge factor (encoder delta); ring boost/cut. Already supported before #21; it's what the Code 61's `pan` encoder layer uses | verified API (mackiecontrol); unit-tested |
| `mute_N`, `solo_N`, `arm_N` | button | `toggle_mixer_param("mute" / "solo" / "record", pos)` on press | verified (mackiecontrol) |
| `select_N` | button | make chain `pos` the active chain | to check (chain_manager API) |
| `bank_prev`, `bank_next` | button | `scroll_h ∓ 8` (clamped to `get_num_filtered_chains()`), then `refresh()` | verified pattern (base class scrolling) |
| `master_volume` | analog / encoder | `"level"` on `pos = -1` (main chain) | verified (mackiecontrol) |
| `master_mute` | button | toggles `"mute"` on the main chain (chain 0); LED lit while muted | unit-tested |
| `track_volume`, `track_pan`, `track_mute`, `track_solo`, `track_arm` | as above | the same mixer params on the **active** chain (its display index) | to check (finding the active chain's index) |
| `strip_prev`, `strip_next` | button | the bank moves by one strip (`mcu: channel_left` / `channel_right` map here now) | unit-tested |
| `track_prev`, `track_next` | button | select the previous / next chain (`ARROW_LEFT` / `ARROW_RIGHT` CUIA in the mixer view is one option) | to check |
| `play` | button | `TOGGLE_AUDIO_PLAY` (or `TOGGLE_MIDI_PLAY`, a driver option) | verified CUIA names (mackiecontrol) |
| `record` | button | `TOGGLE_AUDIO_RECORD` (or `TOGGLE_MIDI_RECORD`) | verified CUIA names |
| `stop` | button | `STOP_AUDIO_PLAY` + `STOP_AUDIO_RECORD` (and the MIDI ones) | verified CUIA names |
| `zynthian.zynpot_1..4` | encoder / analog | Zynthian's four rotary controls on whatever screen is shown: `send_cuia("ZYNPOT", params=[N − 1, delta])`, `ZYNPOT_ABS` for an analog. The Code 61's `enc_zyn` mode puts them on E1–E4 | verified CUIA (mackiecontrol); unit-tested |
| `device_param_1..8` | encoder / analog | the selected chain's current processor's parameters, 4 a page, paged by `device_bank_prev` / `device_bank_next` (§5.3) | unit-tested |
| `device_bank_prev`, `device_bank_next` | button | the device page ∓ ceil(live `device_param_N` / 4), clamped; LED lit while it can move that way (§5.3) | unit-tested |
| `scene_launch`, `scene_prev`, `scene_next`, `stop_all_clips` | button | the zynpad sequencer: `self.zynseq.libseq.togglePlayState(scene, phrase, chan)`, `self.zynseq.scene` | verified API (launchpad_x); the mapping to scenes is to check |
| `clip_T_S` | pad (or button) | zynpad's launcher grid: column T = the T-th chain from the grid's column offset (its MIDI channel picks the sequence), row S = phrase S from the row offset. A press is zynpad's `on_pad_press(phrase, chan)`, the release `on_pad_release`. LED by FB_LED_RGB (§5.1) | unit-tested |
| `grid_left`, `grid_right`, `grid_up`, `grid_down` | button | the grid's column / row offset ∓ 1, clamped; LED lit while it can move that way | unit-tested |
| `nav_back`, `nav_select`, `nav_home`, `nav_alt` | button | `send_cuia("ZYNSWITCH", [n, t])` with n = 1, 3, 0, 2 and t = `S` / `B` / `L` on the release, from the hold time between the EVENTs' `t_ms`, against `zynthian_gui_config.zynswitch_bold_us` / `zynswitch_long_us` (300 ms / 2 s by default; the same comparisons as `zynthian_gui.zynswitch_timing`) | unit-tested |
| `nav_up`, `nav_down`, `nav_left`, `nav_right` | button | `ARROW_UP` / `ARROW_DOWN` / `ARROW_LEFT` / `ARROW_RIGHT` CUIA on press, repeating while held (after ~300 ms, every tick) | unit-tested |
| `nav_scroll` | encoder | `ZYNPOT [3, delta]`: the select knob | unit-tested |
| `zynthian.<cuia>` | button / pad | `send_cuia("<CUIA>")`, the name upper-cased (`zynthian.toggle_audio_play` → `TOGGLE_AUDIO_PLAY`). Another host's `<host>.<command>` is ignored without a log line | unit-tested |
| `shift` | button | tracked as a modifier (`mapping.shift`); no action of its own | unit-tested |
| `loop`, `metronome`, `tap_tempo`, `undo`, `redo`, `rewind`, `fast_forward`, `overdub`, `session_record`, `automation_arm`, `tempo`, `crossfader`, `cue_volume`, `send_a`, `send_b`, `device_prev` / `device_next` / `device_on` | | look for matching CUIAs in your fork's `zynthian_gui` CUIA list; ignore the ones without (log once) | to check |
| `param`, `track_name` (and a display without a role) | display | §5.2 | unit-tested |
| `time`, `status` | display | nothing from Zynthian yet: ignored (logged once) | n/a |
| `meter_N`, `master_meter`, `scene_N`, `knob_N`, `assign_*`, `view_*`, `flip`, `option` / `control` / `alt`, `send_a_N` / `send_b_N`, `master_solo`, `master_select`, markers, zoom, automation, `save`, `time_format` | | not yet: ignored (logged once) | n/a |
| keys | | not mapped: the keys play the synths as normal MIDI | n/a |

**EVENT handling:** a button role acts on **press** (`value` 1) unless it's momentary by nature (e.g. `rewind` held). An analog EVENT carries 0..16383 (use the top bits as needed); an encoder EVENT carries a signed delta in steps (one per detent, more on a quick turn with the device's encoder acceleration, already scaled). A pad EVENT (type 7, from pad twins only) is a press when `value` > 0 and the release at 0; a pad with a button role behaves as that button. Touch EVENTs (type 5) are read and ignored for now.

### 5.1 Clip grid

The pads of a pad layer (`clip_T_S`, e.g. the Code 61's `pads` mode) are a window on the launcher grid Zynthian's mixer and zynpad drivers show:

- **Columns are chains**, in mixer order without the main chain (the driver's filtered chain list, as the strips use). A chain's sequence is the one on its MIDI channel; audio chains (no MIDI channel) have none. **Rows are phrases** of the current scene (`zynseq.scene`).
- **Scrolling:** the driver keeps its own column and row offsets, both 0 at the start and independent of the mixer bank. `grid_left` / `grid_right` move the column offset by one, `grid_up` / `grid_down` the row offset, clamped so the grid stays on chains and phrases that exist (and re-clamped when chains or phrases go).
- **LEDs (FB_LED_RGB, change-only)**, in the colours Zynthian's own pad controllers (Launchpad X) and mixer launchers use, from `zynthian_gui_config`:
  - off (0, 0, 0): no sequence there: no chain or phrase, an audio chain, a disabled sequence (`repeat` 0), or an empty one (no events; for a clippy pad, no clip file), as the mixer's launcher decides "empty";
  - stopped: the sequence group's colour (`LAUNCHER_COLOUR[group]`) at 35 % brightness;
  - playing (`SEQ_PLAYING`, `SEQ_CHILD_PLAYING`): the group's colour at full brightness;
  - start queued (`SEQ_STARTING`): `LAUNCHER_STARTING_COLOUR` (green), blinking;
  - stop queued (`SEQ_STOPPING`, `SEQ_FORCED_STOP`, `SEQ_STOPPING_SYNC`, `SEQ_CHILD_STOPPING`): `LAUNCHER_STOPPING_COLOUR` (red), blinking;
  - recording (`SEQ_RECORDING`, or the pad `state_manager.midi_record_pad` names): `LAUNCHER_REC_COLOUR` (red); blinking while a punch in or out is queued (`SEQ_STARTING_RECORD`, `SEQ_STOPPING_RECORD`). A take shows on an empty pad too, as on a Launchpad: a clippy pad has no clip file until its take is saved after punch-out, so the "empty" test above comes after the record states. Only a disabled pad stays off.

  Colours are #RRGGBB halved to 0..127. Full-colour LEDs (`maudio_pad_leds`, once called `rgb_serial`: the Code 61's pads; `ws2812` strips) show them as given, through the device's brightness curve; other LEDs light from them as sysex.md §4.6.2 says, so on a single-colour LED stopped and playing look alike. Without Zynthian's table the driver falls back to teal for groups and its own green / red.
- **State** is read from `zynseq.state["scenes"][scene]["phrases"][phrase]["sequences"][chan]` (`state`, `repeat`, `group`, `nEvents`), the dict zynpad and the mixer's launchers use; a clippy pad's clip from its chain's clippy processor (`controllers_dict["file <phrase+1>"]`).
- **Refresh:** `update_seq_state` (zynpad's `SS_SEQ_PLAY_STATE` hook) marks the state dirty, so the change shows within 40 ms (§6.1); `SS_SEQ_STATE` (state reloaded) calls `refresh()`; the 100 ms tick polls the rest (e.g. a pattern that gained events).
- **A press** calls zynpad's `on_pad_press(phrase, chan)`, so a pad behaves as on a Launchpad: it toggles the sequence, records in session-record mode, and a hold until the long-press time clears a pad in a record state (`on_pad_release`). The release goes to the pad that was pressed, even if the grid scrolled in between. A press where there's no launcher does nothing.

### 5.2 Displays

- **`param`:** the selected chain's name; while a knob turns, `"<short name> <value>"` (`V1 80%`, `P1 L20`, `M 80%`), or the value alone when both don't fit the display's `cols` (`L100`, `125bpm`). A processor parameter (`device_param_N`) shows its controller's `short_name`, cut to fit before its value (`Cut 64`), and the value alone when there's no room (`Pulse`). `device_bank_prev` / `next` show the new page's first parameter. The text goes out with the next flush, at most every 40 ms, so a knob swept fast sends the latest text, not one per EVENT. About 1.5 s after the last move (15 ticks) the chain name comes back, and at once when the selection or the bank changes. Zynpot targets (`zynthian.zynpot_N`, `nav_scroll`) show nothing: Zynthian's screen shows their value.
- **`track_name`, and a display without a role** (all displays on firmware before display roles): the selected chain's name.
- Text is printable ASCII (accents dropped), cut and padded to `cols`, row 0, sent only when it changes.

### 5.3 Device parameters

`device_param_N` drives the parameters of the **selected chain's current processor** (`chain_manager.get_active_chain().current_processor`, the processor `zynthian_gui.get_current_processor()` falls back to), not the zynpots:

- **The parameter list** is the processor's control screens (`get_ctrl_screens()`, in order), 4 controllers each, concatenated. A screen with fewer than 4 is padded with gaps, so each page of 4 is one screen as Zynthian's control view shows it; a screen without controllers is skipped, as the control view skips it. A gap maps to nothing (ring off).
- **Paging:** the driver keeps a device page (in units of 4). `device_param_N` is parameter `page × 4 + N − 1`. `device_bank_prev` / `device_bank_next` move the page by ceil(the number of live `device_param_N` roles / 4), each N counted once: 1 in the Code 61's `enc_zyn` mode (`device_param_1..4` on E5–E8), 2 in `enc_device` (`device_param_1..8`). The page is clamped to the parameters that exist (and re-clamped when the screens shrink), and goes back to 0 when the selected chain or its current processor changes.
- **Liveness** comes from GET_COMPONENT's `layer` byte and EVENT type 8 (`protocol/sysex.md` §4.6): a component with `layer` n > 0 is live only while mode n − 1 is on. The device replays EVENT 8 for the modes that are on when the driver sets `debug.events` to 1, so the driver needs no extra request. Firmware without the `layer` byte counts every component as live. A role on several twins (`device_param_1` on E5 in `enc_zyn` and on E1 in `enc_device`) works from whichever is live: each is its own component.
- **Turning** calls the controller's own `nudge(delta)` (its nudge factor; one step for a list). An analog or `ZYNPOT_ABS`-style absolute move sets the value across the controller's `value_min`..`value_max`, snapped to the nearest tick for a list. Read-only and file-path controllers are left alone (nudging a path opens Zynthian's file selector).
- **Feedback** is as for other parameters: FB_VALUE to an absolute encoder (twins are absolute by default) and to pick-up analogs, FB_RING to an encoder with a ring, change-only.

## 6. Feedback: Zynthian state → the device

| What changed in Zynthian | Signal / source | Send |
|---|---|---|
| a mixer strip's mute/solo/record/level/balance | `update_mixer_strip(chan, symbol, value)` (zynmixer base, `S_MIXER / SS_ZYNMIXER_SET_VALUE`): marks the state dirty, coalesced (§6.1) | FB_LED for buttons with `mute_N` / `solo_N` / `arm_N` (and `track_*`) on that strip; FB_VALUE (`takeover` = 1 only) for `volume_N` / `pan_N` / `master_volume` |
| the bank scrolled | the driver's own `scroll_h` | re-send everything strip-related for the new bank (FB_LEDS where consecutive) |
| audio player / recorder state | `S_AUDIO_PLAYER / SS_AUDIO_PLAYER_STATE`, `S_AUDIO_RECORDER / SS_AUDIO_RECORDER_STATE`; `state_manager.status_audio_player`, `state_manager.audio_recorder.rec_proc` | FB_LED for `play`, `record`, `stop` (verified: mackiecontrol `refresh_audio_transport`) |
| MIDI player / recorder state | `S_STATE_MAN / SS_MIDI_PLAYER_STATE`, `SS_MIDI_RECORDER_STATE`; `status_midi_player`, `status_midi_recorder` | the same, if the driver option uses MIDI transport |
| the active chain | `on_active_chain(id)` | `select_N` LEDs; everything `track_*` |
| chains added/removed/moved, snapshot loaded | `refresh()` (base class) | everything |
| sequencer pad/phrase state | zynpad (`update_seq_state` in `zynthian_ctrldev_zynpad`), and the tick | FB_LED_RGB for `clip_T_S` pads (§5.1) |
| the selected chain, a knob moved | `on_active_chain`, EVENTs | FB_TEXT to `param` / `track_name` displays (§5.2) |

- An encoder with a ring (`ring` > 0) mapped to a parameter gets **FB_RING**: boost/cut (mode 1) for bipolar parameters (balance), wrap (mode 2) otherwise, `value` 0..16383 = the parameter's position. Use flags bit 1 (ring off) when its role maps to nothing in the current bank. It is sent only when the LEDs it lights change (`ring_lit` in `mapping.py`, the firmware's rule from daw-scripts.md §5.12).
- **Send only changes.** The device takes notifications as fast as USB carries them, but Zynthian's level meters and similar can change often. Don't map meters to LEDs.

### 6.1 Update rate

The driver is change-only, with one cache per LED, RGB LED, value, ring and display, and empties them on BOOTED and `refresh()`. The items from the 2026-11-05 review are done (2026-11-10), following daw-scripts.md §5.12:
- **Coalesced refreshes.** `update_mixer_strip`, `update_seq_state` and `on_active_chain` only mark the state dirty (`request_flush()`). The session thread refreshes once for all of them, at most every 40 ms (`FLUSH_SECONDS`, 25 a second), besides the 100 ms tick. A fader sweep on the touchscreen is one refresh per flush, not one per `SS_ZYNMIXER_SET_VALUE`.
- **One read per refresh.** The mapping reads the song through `_Reads` (as the Live script does): inside one refresh, `bank_tracks()`, `selected_track()`, `device_params()`, `grid_rows()` and the others are read once, however many bindings ask.
- **FB_VALUE after an encoder EVENT** waits for the next flush; the ring moves at once. Ten EVENTs of a fast turn send one FB_VALUE.
- **FB_TEXT** for the `param` display goes out with the flush: the latest text, at most 25 a second.
- **FB_RING** only when the lit LEDs change (above).

Not coalesced: an analog EVENT sets its parameter at once (the device already sends at most one per 7-bit step), and button presses refresh at once.

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
6. **Displays:** done for `param` and `track_name` (§5.2). `time` and `status` need a song position and a message source from Zynthian.
7. **Clip grid on hardware:** how the launcher colours and the 35 % stopped brightness look on the Code 61's pad LEDs, and whether Zynthian's chain order is the order users expect for columns (§5.1).
8. **Auto-long:** Zynthian's own switches fire a long press while still held (`zynswitch_long_us`); nav buttons only time the press on release.
