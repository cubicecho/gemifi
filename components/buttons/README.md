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

## Gestures

A button can do something else when it is held or pressed twice: a **gesture**. A gesture runs an action of its own instead of the button's, from the same list ("Button actions" above).

```yaml
buttons:
  - { id: rec, pin: PB3, action: cc_momentary, cc: 20, hold: { cc: 21 } }   # tap: CC 20; held: CC 21
  - { id: stop, pin: PB4, action: mmc, mmc: stop, hold: panic }             # tap: MMC stop; held: panic
  - { id: next, pin: PB5, action: program_next, double_tap: program_prev }  # tap: next; twice: previous
button_settings:                 # optional; these are the defaults
  tap_time: 200ms
  hold_time: 200ms
```

- **`hold:`** runs when the button is held for `hold_time`. A press let go sooner is a **tap**, and runs the button's own action.
- **`double_tap:`** runs when the button is pressed again within `tap_time` of letting go. The second press runs it, and its release lets go of it. A tap not followed by another within `tap_time` is a single tap. A first press held for `tap_time` (on a button without `hold:`) isn't a tap: the button's own action goes down then, and lets go with the button, so a `cc_momentary` or a `note` still works held. With `hold:` too, holding the first press is the hold. Triple taps aren't supported.
- **What a gesture takes:** an action's name (`hold: panic`), or a mapping with the keys of the button's action: `action:`, `cc:`, `note:`, `preset:`, `program:`, `mmc:`, `sysex:`, `mcu:`. What it leaves out is the button's, so `hold: { cc: 21 }` is the button's `cc_momentary` on CC 21. It sends on the button's `channel:`, with its `min:`, `max:` and `bank:`.
- **Latency.** A button without a gesture fires at the press, as before; only a button with one waits. A tap is known when the button is let go, so its action is pressed and let go together then: a `cc_momentary` tap sends its 127 and its 0 at once. A hold is known after `hold_time`, and lets go when the button does. With `double_tap:`, a single tap waits `tap_time` more after the release, unless another button is pressed first: that press sends the tap at once, before its own action. A double tap is known at the second press.
- **EVENTs** (`debug.events`) still come at the press and the release, as they happen.
- **Nothing hangs.** A hold that sent a note or a CC lets go of it when the button is released, or at a preset switch (`buttons_off`), as a button's own note does. A press still being told apart at a preset switch is dropped: its release sends nothing.
- **Not for every action.** A gesture can't run `cc_ramp`, `bend_down` or `bend_up` (they work for as long as the button is held) or `cc_toggle` (its on/off state is the button's, which its LED and host echoes show). A button whose own action is `cc_ramp` or a pitch bend can't have a gesture, and a preset can't give it one of those actions.
- **The same in every preset**, like modes: presets can't set `hold:` or `double_tap:`. The gestures are YAML only; their timing is the params below.
- **Layer twins** ([modes](../modes/README.md#button-layers)) have no gestures: while a twin is live the switch is the twin's, and fires at the press.
- **Params**, global (stored once, the same in every preset), only on a device with that gesture: `button.tap_time` (50..1000 ms, default 200) and `button.hold_time` (100..5000 ms, default 200), in the "Buttons" group. The YAML's `button_settings:` sets the default; a key for a gesture no button has is a warning.

**Generated code:** `GM_BTN_HOLD` and `GM_BTN_DOUBLE` in `gm_device_config.h` (the buttons with `hold:` and with `double_tap:`, one bit each; 0 or left out: the gesture code isn't compiled in), `GM_BTN_HOLD_TIME_DEFAULT` and `GM_BTN_TAP_TIME_DEFAULT`, and `gm_btn_gestures[GM_N_BUTTONS]` (`gm_device.h` `gm_btn_gesture_t`: each button's `hold` and `double_tap` as `{ action, number }`, `GM_BTN_NO_ACT` for none). The runtime keeps 5 bytes per button (a state and the time of the press or release), only on such a device.

## Combos

Two or more buttons pressed together run an action of their own instead of theirs: a **combo**.

```yaml
combos:
  - { buttons: [b1, b2], action: panic }                    # B1 + B2: panic
  - { buttons: [b1, b2, b3], action: cc_momentary, cc: 90 }
button_settings:
  combo_time: 50ms               # optional; the default
```

- **Together** means each pressed within `combo_time` of the first, in any order. The combo goes down once its buttons are all pressed and no bigger combo with them in it could still come; with one that could, it waits out `combo_time` (or the first release). It lets go when the first of its buttons is let go; the others' releases send nothing.
- **What it takes:** `buttons:` (the YAML's own buttons, 2 or more, each once) and an action with its number, as a gesture takes them ("Gestures"), with `action:` required. It runs on whichever of its buttons comes first in the top-level `buttons:` list, with that button's `channel:`, `min:`, `max:` and `bank:`.
- **Latency.** A button in a combo waits for it: its own press (or its gesture) goes on when the combo can't happen any more: after `combo_time`, when it is let go, when another button is pressed, or when a press can't make a combo with it. Buttons in no combo never wait.
- **Not combos:** a mode's toggle, hold or cycle button, a modifier or a `with:` button (Shift + a button is a mode's job: [modes](../modes/README.md)), a layer twin, or a button whose action is `cc_ramp` or a pitch bend. The actions are those of a gesture. While a button's layer twin is live, its presses are the twin's and don't make the combo.
- **Global**, the same in every preset; `button.combo_time` (10..500 ms, default 50) is a global param in the "Buttons" group, only on a device with combos. A combo that sent a note or a CC lets go of it at a preset switch.

**Generated code:** `GM_N_COMBOS`, `GM_COMBO_BUTTONS` (the buttons in any combo, one bit each), `GM_N_COMBO_BUTTONS` and `GM_BTN_COMBO_TIME_DEFAULT` in `gm_device_config.h`, and `gm_btn_combos[GM_N_COMBOS]` (`gm_device.h` `gm_btn_combo_t`: the buttons as a bit mask and `{ action, number }`). The runtime keeps the waiting presses in order (one byte per combo button) and 13 bytes more.

## Auto-repeat

A stepping button can step again while it is held: `repeat: true`.

```yaml
buttons:
  - { id: oct_up, pin: PB6, action: octave_up, repeat: true }
  - { id: next, pin: PB7, action: preset_next, repeat: true }
button_settings:                 # optional; these are the defaults
  repeat_delay: 500ms
  repeat_rate: 50ms
```

- **What repeats:** `octave_down` / `octave_up`, `transpose_down` / `transpose_up`, `pad_octave_down` / `pad_octave_up`, `program_next` / `program_prev` and `preset_next` / `preset_prev`. The first step is at the press, as always; held for `repeat_delay`, the action runs again every `repeat_rate` until the button is let go. Octave and transpose stop at their limits (both octave buttons held: 0, as before), program and presets go round.
- **It's the button's own action that repeats:** a preset or the host that gives the button another action decides whether it repeats (the action is read again each time), and the YAML's action not being one of these is a warning. A mode's binding of the button (`buttons:`) and a layer twin don't repeat.
- **Through preset switches:** `preset_next` held steps on through the presets it switches to; any held repeat goes on after a preset switch, with that preset's action.
- **No EVENTs** for the repeats: the press and the release are the button's EVENTs.
- **Not with** `hold:` or `double_tap:` (they wait to tell a press apart), nor on a mode's switching button, a modifier or a `with:` button. It may be in a combo: it repeats once its press goes on as its own.
- **Params**, global, only on a device with `repeat:`: `button.repeat_delay` (100..2000 ms, default 500) and `button.repeat_rate` (20..1000 ms between repeats, default 50), in the "Buttons" group.

**Generated code:** `GM_BTN_REPEAT` (the buttons with `repeat: true`, one bit each), `GM_BTN_REPEAT_DELAY_DEFAULT` and `GM_BTN_REPEAT_RATE_DEFAULT` in `gm_device_config.h`. The runtime keeps the time of the next repeat, 4 bytes per button.

## Gestures or modes?

Gestures give one button (or a set of buttons pressed together) a second action. [Modes](../modes/README.md) change what other buttons do while they are on. Each job has one way to do it:

| You want | Use |
|---|---|
| a second action on the same button, by holding it | `hold:` |
| ... by pressing it twice | `double_tap:` |
| two or more buttons pressed together to do something else | `combos:` |
| Shift + a button to do something else (Shift keeps its own action) | a modifier: a `hold:` mode with a `layer:` |
| Shift + a button to turn a layer on and off | `toggle: { button, with: shift }` |
| a layer for the next press only | a `one_shot:` mode |
| other buttons changing while one is held, or until it is pressed again | a `hold:` or `toggle:` mode, or a cycle |
| a stepping button that keeps stepping | `repeat: true` |

The builder refuses the overlaps: gestures, `repeat:` and combos on a mode's toggle, hold, one-shot or cycle button (it has no action of its own) and on a modifier or a `with:` button (held for Shift + a button, which is the mode's job; a combo of Shift and B would be a second way to say it). A mode that takes a press (its toggle, a `buttons:` binding while it is on, a layer twin) takes it at once, without the gestures.

## Files

- `buttons.c`: sampling and debouncing the buttons, their actions, the CC ramps and the pitch-bend buttons' glide. Built when the device has buttons.
- `__init__.py`: the builder part: the actions (`ACTION_IDS`), `validate` (its YAML), `button_action` (a button's action and number, also for a preset's override and a layer twin) and `c_tables` (`gm_button_t buttons[]`), and when it's built.

## How it runs

The main loop (`gm_poll`) calls:
- `buttons_sample` every `BTN_SAMPLE_MS` (1 ms). A press counts at the first sample that read it closed in every scan since the sample before, so a quick tap is never lost. A release counts once no scan has read it closed for `BTN_STABLE` (16) samples in a row (16 ms), which rides out contact bounce: a contact that flickers during a light press stays one press. A button on a switch matrix is scanned several times per sample (every frame with the hardware scan), so one noisy scan doesn't press it either.
- `ramps_ms` once per elapsed millisecond: the `cc_ramp` buttons glide towards 127 or 0, and the pitch-bend buttons' bend towards its target.

Each press or release goes, in this order:
1. to the safety combos (`gm_safety_button`);
2. to the live one of a button and its layer twins; a press then first sends a single tap still waiting for a double tap, and decides a combo it isn't in (`gestures_before_press`);
3. to the modes: a mode's toggle, hold or cycle button (`mode_toggle_button`), or a function button of the active mode (`mode_function_button`);
4. to the gestures, for a button with one or in a combo (`gesture_input`, "Gestures", "Combos"): `gestures_ms`, at the end of each sample, times them;
5. to the button's own action (`button_changed`), and with `repeat:` its repeats ("Auto-repeat": `repeats_ms`, at the end of each sample).

A preset switch (`gm_release_all`) calls `buttons_off`, which lets go of every note and CC the buttons hold. Buttons held through it release silently.

## What it shares

Through `core/include/gm_controls.h`:
- `btn_held`, `btn_toggled` and `ramp_q8`, which the LEDs show (host feedback also sets `btn_toggled`);
- `btn_stable`, for `gm_poll`, the LED walk and the idle time (`gm_idle_ms`);
- `btn_live`, the buttons that take a press, which the modes (`layers_update`) write;
- `btn_hist`, the samples, for the raw readings (`GM_MONITOR`);
- `step_and_show`, which the modes' octave and transpose bindings use, and `BTN_CH`.

## Tests

In `core/test/`: `test_controller.c` (debounce, actions, ramps, presets), `test_modes.c` (modes, layers, cycles), `test_msg.c` (program, MMC, SysEx and panic actions) and `test_gestures.c` (the gestures, on fake time, with `gestures-test.yaml`). The builder's side: `tools/builder/tests/test_gestures.py`.
