# DAW scripts (host feedback, F3)

Scripts that run inside a DAW and drive gemifi devices with the host feedback protocol (`protocol/sysex.md` §4.6): they find a device, read its components with GET_COMPONENT, hear presses through EVENT messages, and drive LEDs, values, rings and text with the FB_* notifications.

**Writing a new one?** Start from [`protocol/daw-scripts.md`](../protocol/daw-scripts.md): the base for every gemifi DAW integration (the session, every message with example frames, the behaviour rules, a checklist). Put host-specific notes and code in a directory here.

| Directory | Host |
|---|---|
| `ableton-live/` | Ableton Live 11/12 Remote Script (Python 3). Unit-tested, not yet tried in Live; see its README |
| `bitwig/` | Bitwig Studio 5+ controller extension (Java 17, Maven, `extension-api` 25). Unit-tested, not yet tried in Bitwig; see its README |
| `zynthian/` | Notes for a Zynthian "ctrldev" driver (GPL, so it lives in a zynthian-ui fork). No code here |

## Roles

A component's `role` (YAML `daw:`, else its `mcu:` name) says what a script connects it to. The builder accepts these names, plus anything starting with `x_` for a script's own use. A script ignores roles it doesn't know.

| Kind | Roles |
|---|---|
| Transport (buttons, and LEDs that show the state) | `play`, `stop`, `record`, `loop`, `metronome`, `overdub`, `session_record`, `automation_arm`, `tap_tempo`, `undo`, `redo`, `rewind`, `fast_forward` |
| Selected track (buttons / LEDs) | `track_prev`, `track_next`, `track_mute`, `track_solo`, `track_arm` |
| Selected track (analog / encoders) | `track_volume`, `track_pan`, `send_a`, `send_b` |
| Mixer strips 1–8, in the current bank (buttons / LEDs) | `mute_N`, `solo_N`, `arm_N`, `select_N`, `bank_prev`, `bank_next` |
| Mixer strips 1–8 (analog / encoders) | `volume_N`, `pan_N` |
| Master (analog / encoders) | `master_volume`, `cue_volume`, `crossfader`, `tempo` (encoder) |
| Selected device (analog / encoders) | `device_param_N` (1–8) |
| Selected device (buttons / LEDs) | `device_prev`, `device_next`, `device_bank_prev`, `device_bank_next`, `device_on` |
| Session (buttons / LEDs) | `scene_launch`, `scene_prev`, `scene_next`, `stop_all_clips` |

`N` is 1–8. An LED with a role shows that function's state (e.g. `record` lit while recording). A button with a role and an LED shows the same state on its LED.

`mcu:` names used as roles map like this: `fader_N` → `volume_N`, `master` → `master_volume`, `vpot_N` → `pan_N`, `rec_N` → `arm_N`, `solo_N`, `mute_N`, `select_N` as they are, `bank_left` / `bank_right` → `bank_prev` / `bank_next`, `channel_left` / `channel_right` → `track_prev` / `track_next`, and `play`, `stop`, `record`, `rewind`, `fast_forward`, `cycle` (→ `loop`), `click` (→ `metronome`), `undo` as transport.
