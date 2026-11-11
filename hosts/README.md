# DAW scripts (host feedback, F3)

Scripts that run inside a DAW and drive gemifi devices with the host feedback protocol (`protocol/sysex.md` §4.6): they find a device, read its components with GET_COMPONENT, hear presses through EVENT messages, and drive LEDs, values, rings and text with the FB_* notifications.

**Writing a new one?** Start from [`protocol/daw-scripts.md`](../protocol/daw-scripts.md): the base for every gemifi DAW integration (the session, every message with example frames, the behaviour rules, a checklist). Put host-specific notes and code in a directory here.

| Directory | Host |
|---|---|
| `ableton-live/` | Ableton Live 11/12 Remote Script (Python 3). Unit-tested, not yet tried in Live; see its README |
| `bitwig/` | Bitwig Studio 5+ controller extension (Java 17, Maven, `extension-api` 25). Unit-tested, not yet tried in Bitwig; see its README |
| `zynthian/` | Notes for the Zynthian "ctrldev" driver. The driver is GPL, so it lives in the zynthian-ui fork [`vantreeseba/zynthian-ui`](https://github.com/vantreeseba/zynthian-ui) (`ui-modernize`). Unit-tested, not yet tried on a device. No code here |

## Roles

A component's `role` (YAML `daw:`, else its `mcu:` name) says what a script connects it to. The builder accepts these names, plus anything starting with `x_` for a script's own use and `<host>.<command>` (below). A script ignores roles it doesn't know. What each role does: [`protocol/daw-scripts.md`](../protocol/daw-scripts.md) §5–§6.

| Kind | Roles |
|---|---|
| Transport (buttons, and LEDs that show the state) | `play`, `stop`, `record`, `loop`, `metronome`, `overdub`, `session_record`, `automation_arm`, `tap_tempo`, `undo`, `redo`, `rewind`, `fast_forward` |
| Markers and edit (buttons / LEDs) | `marker_prev`, `marker_next`, `marker_add`, `nudge_back`, `nudge_forward`, `zoom_in`, `zoom_out`, `scrub`, `save`, `time_format` |
| Automation mode (buttons / LEDs) | `automation_read`, `automation_write`, `automation_touch`, `automation_latch`, `automation_off` |
| Navigation (buttons) | `nav_up`, `nav_down`, `nav_left`, `nav_right`, `nav_select`, `nav_back`, `nav_home`, `nav_alt` |
| Navigation (encoder) | `nav_scroll` |
| Modifiers (buttons) | `shift`, `option`, `control`, `alt` |
| Selected track (buttons / LEDs) | `track_prev`, `track_next`, `track_mute`, `track_solo`, `track_arm` |
| Selected track (analog / encoders) | `track_volume`, `track_pan`, `send_a`, `send_b` |
| Mixer strips 1–8, in the current bank (buttons / LEDs) | `mute_N`, `solo_N`, `arm_N`, `select_N`, `bank_prev`, `bank_next` (by 8), `strip_prev`, `strip_next` (by 1) |
| Mixer strips 1–8 (analog / encoders) | `volume_N`, `pan_N`, `send_a_N`, `send_b_N`, `knob_N` (strip N's assignable knob) |
| Knob assign and view (buttons / LEDs) | `assign_pan`, `assign_send`, `assign_device`, `assign_eq`, `assign_track`, `assign_instrument`, `flip`, `view_all`, `view_midi`, `view_audio`, `view_inputs`, `view_buses` |
| Master (analog / encoders) | `master_volume`, `cue_volume`, `crossfader`, `tempo` (encoder) |
| Master (buttons / LEDs) | `master_mute`, `master_solo`, `master_select` |
| Selected device (analog / encoders) | `device_param_N` (1–8), `device_preset`, `device_preset_bank` (encoders: the preset / its bank ±) |
| Selected device (buttons / LEDs) | `device_prev`, `device_next`, `device_bank_prev`, `device_bank_next`, `device_on` |
| Session (buttons / LEDs) | `scene_launch`, `scene_prev`, `scene_next`, `stop_all_clips` |
| Clip grid (pads or buttons / LEDs) | `clip_T_S` (track T, scene S, 1–8, from the grid's scroll position), `scene_N` |
| Clip grid scroll (buttons) | `grid_up`, `grid_down`, `grid_left`, `grid_right` (by 1) |
| Displays | `time`, `track_name`, `param`, `status` |
| Meters (reserved: no firmware has meters yet) | `meter_N`, `master_meter` |
| Host commands (any kind) | `<host>.<command>`, e.g. `zynthian.toggle_audio_play` (a button), `zynthian.zynpot_N` (an encoder: Zynthian's knob N, 1–4) |

`N` is 1–8. An LED with a role shows that function's state (e.g. `record` lit while recording). A button or pad with a role and an LED shows the same state on its LED.

**Host commands.** `<host>.<command>` is lower case, `[a-z][a-z0-9]*\.[a-z0-9_]+`. It runs one of that host's own commands. The hosts: `live`, `bitwig`, `zynthian`. On an encoder it is a knob of the host's own: `zynthian.zynpot_1` .. `zynthian.zynpot_4` turn Zynthian's four knobs (the Code 61's ZYN encoder mode puts them on E1–E4). A script handles only its own namespace and ignores the others without logging them.

`mcu:` names used as roles map like this: `fader_N` → `volume_N`, `master` → `master_volume`, `vpot_N` → `pan_N`, `rec_N` → `arm_N`, `solo_N`, `mute_N`, `select_N` as they are, `bank_left` / `bank_right` → `bank_prev` / `bank_next`, `channel_left` / `channel_right` → `strip_prev` / `strip_next`, and `play`, `stop`, `record`, `rewind`, `fast_forward`, `cycle` (→ `loop`), `click` (→ `metronome`), `undo` as transport.
