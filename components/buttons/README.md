# buttons

A control ([core/README.md "Controls"](../../core/README.md#controls)): buttons and switches on any input pin (an MCU pin, a hub's, a switch matrix's), each with an action.

```yaml
buttons:
  - { id: button_1, pin: { number: PB12, mode: INPUT_PULLUP, inverted: true }, action: cc_toggle, cc: 80, led: led }
  - { id: button_2, pin: { number: PB13, mode: INPUT_PULLUP, inverted: true }, action: cc_momentary, cc: 64 }
  - { id: button_3, pin: { number: PB14, mode: INPUT_PULLUP, inverted: true }, action: octave_down }
```

Its YAML (`buttons:`): the fields in [core/README.md "Controls"](../../core/README.md#controls), "Button actions" below, the message actions in [core/README.md "MIDI messages"](../../core/README.md#midi-messages), and the modes and layers in [modes](../modes/README.md).

## Button actions

`button.action` values (YAML names in brackets): 0 none (`none`), 1 CC momentary (`cc_momentary`), 2 CC toggle (`cc_toggle`), 3 CC ramp (`cc_ramp`: glides to 127 while held, back to 0 on release), 4 note (`note`), 5 octave down (`octave_down`), 6 octave up (`octave_up`), 7 transpose down (`transpose_down`), 8 transpose up (`transpose_up`), 9 pitch bend down (`bend_down`), 10 pitch bend up (`bend_up`), 11 program next (`program_next`), 12 program previous (`program_prev`), and on devices with presets (2 or more slots; without them presets are off, these options are left out of the schema and the YAML refuses them, see [core/README.md "Presets"](../../core/README.md#presets)) 13 next preset (`preset_next`), 14 previous preset (`preset_prev`), 15 select preset (`preset_select`: `button.number` is the slot, 1..slots, from the YAML's `preset:`, e.g. `{ action: preset_select, preset: 3 }`), and on devices with `mcu:` only, 16 Mackie Control (`mcu`; it was 13 before presets, so a profile stored with it needs setting again). `button.number` is the CC or note number: the YAML's `cc:` for the CC actions, `note:` for `note`, the slot for `preset_select`, the MCU note for `mcu` (from `mcu: play` etc.). The MIDI message actions, only on devices that use them ([core/README.md "MIDI messages"](../../core/README.md#midi-messages)): 17 program (`program`, `program:` 0..127), 18 panic (`panic`), 19 MMC (`mmc`, `mmc: play` etc.), 20 SysEx (`sysex`, `sysex:` a string's id). On devices with pads: 21 pad octave down (`pad_octave_down`), 22 pad octave up (`pad_octave_up`): `pad.octave` ∓ 1, both held: back to 0, as the octave buttons. Other actions take neither (a `cc:`/`note:` there only sets the default number for when the action is changed in the config, with a warning).

## Files

- `buttons.c`: sampling and debouncing the buttons, their actions, the CC ramps and the pitch-bend buttons' glide. Built when the device has buttons.
- `__init__.py`: the builder part: the actions (`ACTION_IDS`), `validate` (its YAML), `button_action` (a button's action and number, also for a preset's override and a layer twin) and `c_tables` (`gm_button_t buttons[]`), and when it's built.

## How it runs

The main loop (`gm_poll`) calls:
- `buttons_sample` every `BTN_SAMPLE_MS` (1 ms). A press counts at the first sample that read it closed in every scan since the sample before, so a quick tap is never lost. A release counts once no scan has read it closed for `BTN_STABLE` (16) samples in a row (16 ms), which rides out contact bounce: a contact that flickers during a light press stays one press. A button on a switch matrix is scanned several times per sample (every frame with the hardware scan), so one noisy scan doesn't press it either.
- `ramps_ms` once per elapsed millisecond: the `cc_ramp` buttons glide towards 127 or 0, and the pitch-bend buttons' bend towards its target.

Each press or release goes, in this order:
1. to the safety combos (`gm_safety_button`);
2. to the live one of a button and its layer twins;
3. to the modes: a mode's toggle, hold or cycle button (`mode_toggle_button`), or a function button of the active mode (`mode_function_button`);
4. to the button's own action (`button_changed`).

A preset switch (`gm_release_all`) calls `buttons_off`, which lets go of every note and CC the buttons hold. Buttons held through it release silently.

## What it shares

Through `core/include/gm_controls.h`:
- `btn_held`, `btn_toggled` and `ramp_q8`, which the LEDs show (host feedback also sets `btn_toggled`);
- `btn_stable`, for `gm_poll`, the LED walk and the idle time (`gm_idle_ms`);
- `btn_live`, the buttons that take a press, which the modes (`layers_update`) write;
- `btn_hist`, the samples, for the raw readings (`GM_MONITOR`);
- `step_and_show`, which the modes' octave and transpose bindings use, and `BTN_CH`.

## Tests

In `core/test/`: `test_controller.c` (debounce, actions, ramps, presets), `test_modes.c` (modes, layers, cycles) and `test_msg.c` (program, MMC, SysEx and panic actions).
