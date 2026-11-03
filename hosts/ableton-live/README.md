# gemifi for Ableton Live (Remote Script)

A Live 11/12 control surface script that drives **any** gemifi device with host feedback (HELLO caps bit 6, `protocol/sysex.md` §4.6). It learns the device's controls with GET_COMPONENT, hears presses and moves through EVENT messages, and lights LEDs, sets pick-up targets and drives encoder rings with the FB_* notifications. Nothing in it is specific to one device: the component `role`s (`hosts/README.md`) say what each control does.

Written from scratch against the public Live Object Model (as documented for Max for Live) and the `c_instance` interface. It contains no Ableton code and is not derived from Ableton's MIDI Remote Scripts. MIT licensed.

**Status: untested in real Live.** Everything below was written and unit-tested without Live (see "Tests" and "Open questions").

## Install

1. Copy the `gemifi/` folder into Live's User Library, under `Remote Scripts`:
   - macOS: `~/Music/Ableton/User Library/Remote Scripts/gemifi`
   - Windows: `%USERPROFILE%\Documents\Ableton\User Library\Remote Scripts\gemifi`
   (Create `Remote Scripts` if it isn't there.)
2. Restart Live.
3. Preferences → Link, Tempo & MIDI → Control Surface: pick **gemifi**. Input and Output: the device's **main** port (the one `tools/host/gemifi.py` talks to), not its DAW/Mackie port.
4. In the MIDI Ports list, leave **Track** on for that input if you want its keys, pads and CCs to keep playing instruments and to be MIDI-mappable. The script maps none of them; it only reads SysEx.

The script talks to whatever ports are assigned to the "gemifi" slot; with two devices, use two slots. When it finds the device, Live's status bar shows `gemifi: connected to <node> (fw x.y.z)`. Diagnostics go to Live's `Log.txt`, prefixed `gemifi:`.

## What it does

1. On the first `update_display` (Live calls it about every 100 ms), it sends HELLO. No answer: it tries again about every 3 s. A device without caps bit 6 (older firmware, or the bootloader) gets no further messages.
2. GET_COMPONENT for each kind (button, keys, analog, encoder, LED, display) from index 0 up to `count`; BAD_PARAM at index 0 = none of that kind.
3. Reads `debug.events` (param 13), then sets it to 1.
4. Ready: every tick it compares the song's state with what it last sent and sends only the differences (FB_LED, or FB_LEDS for runs of neighbouring LEDs; FB_VALUE; FB_RING). FB_VALUE goes only to analog inputs with `takeover` pick-up and to absolute encoders, and LEDs with `owner` local get nothing (GET_COMPONENT's added fields; firmware without them gets everything, as before).
5. When the device sends BOOTED (§4.6.3: it restarted, so its FB_* states are gone), it starts again from HELLO and re-sends everything. As a fallback it reads `debug.events` back: every ~5 s from firmware without BOOTED, every ~30 s from firmware with it (BOOTED can be missed if the port opens late). A timeout or a 0 means the device was unplugged or restarted, so it starts again from HELLO.
6. On exit (`disconnect`) it sends FB_RELEASE 0 and puts `debug.events` back to what it was.

Requests follow §1's stop-and-wait (one outstanding, same `seq` on a resend, up to 6 sends). The timeout is 3 ticks (≥ 200 ms). Notifications go out at any time, only to a device with caps bit 6.

## Roles

`mcu:` names are mapped first, as `hosts/README.md` says (`fader_N` → `volume_N`, `vpot_N` → `pan_N`, `rec_N` → `arm_N`, `master` → `master_volume`, `bank_left/right`, `channel_left/right`, `cycle` → `loop`, `click` → `metronome`). Unknown roles are logged and ignored; `x_…` roles are ignored silently.

A role on a button acts on press, and the button's LED (if it has one) shows the state. A role on a kind-5 LED shows the same state. LED levels: 127 on, 0 off, 1 blink.

| Role | Button press | LED |
|---|---|---|
| `play` | toggle play / stop | lit while playing |
| `stop` | stop | lit while held |
| `record` | toggle arrangement record | lit while recording, **blinks** while armed but stopped |
| `loop`, `metronome`, `overdub`, `session_record`, `automation_arm` | toggle (Song `loop`, `metronome`, `arrangement_overdub`, `session_record`, `session_automation_record`) | lit while on |
| `tap_tempo`, `undo`, `redo` | tap / undo / redo | lit while held |
| `rewind`, `fast_forward` | jump 4 beats; repeats every tick after ~300 ms held | lit while held |
| `track_prev`, `track_next` | select the previous / next track (visible tracks, returns, master) | lit if there is one |
| `track_mute`, `track_solo`, `track_arm` | toggle on the selected track | state of the selected track |
| `mute_N`, `solo_N`, `arm_N` | toggle on strip N of the bank | state of that track |
| `select_N` | select strip N's track | lit when it is the selected track |
| `bank_prev`, `bank_next` | move the 8 strips by 8 tracks | lit if possible |
| `device_prev`, `device_next` | select the previous / next device on the selected track | lit if possible |
| `device_bank_prev`, `device_bank_next` | page `device_param_1..8` by 8 parameters | lit if possible |
| `device_on` | toggle the selected device on / off | lit while on |
| `scene_launch` | fire the selected scene | lit while held |
| `scene_prev`, `scene_next` | select the previous / next scene | lit if possible |
| `stop_all_clips` | stop all clips | lit while held |

Continuous roles work on analog inputs (absolute, 0..16383) and encoders (signed deltas):

| Role | Target |
|---|---|
| `track_volume`, `track_pan`, `send_a`, `send_b` | the selected track's mixer |
| `volume_N`, `pan_N` | strip N of the current bank |
| `master_volume`, `cue_volume`, `crossfader` | the master track's mixer |
| `tempo` | song tempo: 1 BPM per encoder step (20–999 BPM over an analog's travel) |
| `device_param_N` | parameter N of the current page of the selected device (its on/off switch is left out; it's `device_on`) |

- **Analog inputs** use pick-up: after the target changed in Live (mouse, automation, a bank switch), the knob does nothing until it comes within 3 % of the value or crosses it. The script also sends the value as FB_VALUE, so a device with `takeover: pickup` does the same for its own MIDI.
- **Encoders** move a parameter by 1/128 of its range per step (quantized parameters: one value per detent), and get FB_VALUE (for absolute mode) and, with a ring, FB_RING: boost/cut with the centre LED for bipolar parameters (pan, crossfader), wrap otherwise; wrap at 0 for a control with no target.
- Keys (kind 2) and displays (kind 6) have no roles yet and are left alone.

## Files

| File | |
|---|---|
| `gemifi/__init__.py` | `create_instance(c_instance)`, the entry point Live calls |
| `gemifi/surface.py` | the Remote Script object: Live's callbacks, SysEx in/out, the tick |
| `gemifi/device.py` | device state machine: HELLO, GET_COMPONENT enumeration, `debug.events`, retries, keep-alive, `mcu:` → role |
| `gemifi/mapping.py` | roles → actions and feedback, over an abstract song (`Song`, `Track`, `Param`) |
| `gemifi/live_song.py` | that abstract song on top of the LOM |
| `gemifi/protocol.py` | SysEx codec: frames, 7-bit encodings, lstring, HELLO, GET_COMPONENT, EVENT, FB_* |

`protocol.py`, `device.py` and `mapping.py` don't touch Live, and none of the files imports the `Live` module.

## Tests

```
cd hosts/ableton-live && python3 -m pytest -q
```

No Live needed: a fake device answering per §4.6 (with a lost reply, BUSY, silence, a reboot), a fake song, LOM-shaped stand-ins for `live_song.py`, the vectors in `protocol/test-vectors.json`, and a check that every file parses as Python 3.7.

## Live APIs relied on (and how sure)

Sure enough: `create_instance(c_instance)`; `c_instance.song()`, `send_midi(tuple)`, `show_message()`, `log_message()`; the script callbacks `receive_midi`, `build_midi_map`, `update_display`, `refresh_state`, `disconnect`, `connect_script_instances`, `can_lock_to_devices`. LOM: `Song.is_playing`, `start_playing()`, `stop_playing()`, `record_mode`, `loop`, `metronome`, `arrangement_overdub`, `session_record`, `session_automation_record`, `tempo`, `tap_tempo()`, `undo()`, `redo()`, `can_undo`, `can_redo`, `jump_by()`, `stop_all_clips()`, `tracks`, `visible_tracks`, `return_tracks`, `master_track`, `scenes`; `Song.View.selected_track`, `selected_scene`, `select_device()`; `Track.mute/solo/arm/can_be_armed/mixer_device/devices/view.selected_device`; `MixerDevice.volume/panning/sends/cue_volume/crossfader`; `Device.parameters`; `DeviceParameter.value/min/max/is_quantized/is_enabled`; `Scene.fire()`.

Less sure (every call is wrapped so a missing one does nothing):
- **SysEx reaching `receive_midi`.** The script assumes Live passes all SysEx from the surface's input port to `receive_midi` without a MIDI-map entry, so `build_midi_map` maps nothing. That is isolated in `_route_sysex_to_script()` in `surface.py`; if SysEx doesn't arrive in Live, that's the place to add a forwarding call.
- **`send_midi` from `disconnect`.** FB_RELEASE is sent there; if Live has already closed the port, the device keeps the host states until it restarts.
- **Object identity.** Change detection needs a stable key for tracks, parameters and devices. `lom_key()` uses `_live_ptr` if the object has one, else `id()`. If Live gives a new wrapper per access and there is no `_live_ptr`, pick-up would reset every tick (FB_VALUE resent each tick); look there first.
- `port_settings_changed`, `suggest_input_port`, `suggest_output_port`: provided as harmless no-ops / restarts in case Live calls them.
- `parameters[0]` being the device's on/off switch.

## Open questions

- Not tried in Live 11 or 12 at all: install path, the Control Surface list entry, SysEx in and out, timing of `update_display`.
- Polling at 10 Hz instead of LOM listeners: simple and safe, but a change in Live reaches the LEDs up to 100 ms late. Listeners for song-level state could be added later.
- Spec points in `protocol/sysex.md` §4.6 the script had to guess at:
  - A button's `led` is taken to be an index in the kind-5 (LED) list, and every LED, a button's own included, is assumed to appear there.
  - Whether EVENTs for an analog input with `takeover: pickup` are held back until it picks up. The script does its own pick-up either way.
  - An encoder's `mode` can change while the script runs (SET_PARAM `encoder.mode`, a preset change); the script reads it once, at connect.
  - Encoder delta units (per detent? accelerated?) aren't specified; the script assumes one unit per detent.
  - There's no "ring off". The script sends wrap mode with value 0 and assumes it lights nothing.
  - `debug.events` is volatile (never stored), so a COMMIT while the script runs or a script crash can't leave it on after a reboot. The script still restores the old value on exit.
  - A device restart drops the FB_* states; the script hears BOOTED, or (older firmware, or a BOOTED sent before the port was open) notices by polling `debug.events`.
