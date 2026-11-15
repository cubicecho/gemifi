# modes

A control ([core/README.md "Controls"](../../core/README.md#controls)): modes that change what keys and buttons do while they're on, entered with a toggle, hold or cycle button. Its YAML (`modes:`): "Modes", "Button layers" and "Cycles" below.

- `modes.c`: which modes are on, their key and button bindings, the layers and cycles, and the LEDs that show them. Built when the device has modes.
- `__init__.py`: the builder part: `validate` (its YAML: toggle, hold and cycle buttons, keys, buttons, layers and their twins, steps) and `c_tables` (`gm_modes[]`, the key and button tables, `gm_mode_rgb[]`), and when it's built.

## Modes

A mode is a layer that turns the keys (and buttons) into function keys: while it is active, a key press runs what the mode binds to that key and sends no note (research/core/presets-design.md §6). It is how a keyboard with few buttons switches presets, the octave or the channel. Modes are device UI, so they are the same in every preset: a preset can't take away the way back.

**No keyboard needed.** A device with buttons and no `key_matrix` (a pad box, a fader box, a floor board) has modes too: toggle and hold modes with `buttons:`, `with:` (Shift) combinations, layers ("Button layers") and cycles ("Cycles"). Only `keys:` needs a key matrix, and with it the sub-modes (`parent:`), which are entered from a key. A mode needs a button to enter it (`toggle:`, `hold:` or `cycle:`).

```yaml
# an FCB1010-style floor board: sw1 and sw2 send programs 1 and 2 (action: program)
modes:
  - id: bank_b
    toggle: bank                 # a layer-only toggle: programs 6 and 7 while it is on
    layer: { sw1: { program: 6 }, sw2: { program: 7 } }
  - id: banks
    cycle: page                  # each press: the next step
    steps:
      - { id: live, name: LIVE }
      - { id: fx, name: FX, layer: { sw1: { action: cc_toggle, cc: 80 } } }
  - id: setup
    hold: tuner                  # while Tuner is held, the switches pick the channel
    buttons: { sw1: channel_down, sw2: channel_up }
```

```yaml
modes:
  - id: edit
    toggle: { button: edit }     # this button enters the mode, and leaves it (from any sub-mode); or `toggle: edit`
    led: edit_led                # lit while the mode is active (default: the toggle button's LED)
    preset_blink: [oct_down_led, oct_up_led]   # optional: after a preset switch, the first blinks the slot number,
                                               # then both flash once (sub-modes inherit it)
    keys:                        # the physical key, by its note at octave 0 / transpose 0 (C4 = 60), or a MIDI note number
      C3: octave_reset           # an action without a number: just its name
      C#3: octave_down
      E3: { mode: preset_pick }  # go to a sub-mode
      F3: preset_prev
      G3: preset_next
      A3: { action: channel, channel: 10 }
      C5: { action: velocity_curve, curve: hard }
  - id: preset_pick
    parent: edit                 # a sub-mode: entered from a key, back to its parent after one action
    led: { edit_led: blink }     # default for a sub-mode: its parent's LED, blinking
    keys: { white_from: D4, action: preset_select }   # D4, E4, F4, ... = preset 1, 2, 3, ...
  - id: channel
    hold: { button: channel }    # active while the button is held (or `hold: channel`)
    buttons:                     # what these buttons do instead, while the mode is active
      up: channel_up
      down: channel_down
```

- **Entering and leaving.** The toggle button enters the mode and, pressed again, leaves it, also from one of its sub-modes; there is no timeout. A `hold:` mode is active only while its button is held (it can't have sub-modes); it can be held during a toggled mode, and goes first.
- **Which modes can be on together.** The *key modes*, toggle modes with `keys:`, `buttons:` or sub-modes, take turns: entering one leaves the other. A *layer-only toggle mode* (a toggle mode with only a `layer:`, "Button layers") and each cycle ("Cycles") have their own state, so any number of them can be on at once, with each other and with a key mode or a held hold mode. Keys held when the mode starts get their Note Off then, and their release sends nothing. A sub-mode (`parent:`) goes back to its parent after one action, so Edit → E3 → D4 picks preset 1 and leaves you in edit mode for another action, until Edit is pressed again.
- **`toggle: { button: <id>, with: <id> }`** toggles the mode only while the `with:` button is held (Shift + Button mode, say). A plain press runs the toggle button's own action, so it keeps its `action:` and presets can change it; a press that toggled the mode sends nothing on release. `with:` is for `toggle:` only.
- **A modifier** is a `hold:` mode with only a `layer:` (no `keys:` or `buttons:`), like Shift: its button keeps its own action (presets can change it), and while it is held the layer's twins take their buttons' presses, so each twin is that button's "Shift +" action ("Button layers"). It can also be the `with:` of toggles and cycles: Shift + Button mode still toggles, so a button that the modifier toggles a mode with can't have a twin in its layer. Its button can't be bound in a mode, have a twin or enter another mode. Two hold modes held at once: the last pressed is live, and letting go of it goes back to the other.

```yaml
modes:
  - id: shifted
    hold: shift                  # Shift still sends its CC (and keeps its daw: role)
    layer:
      up: { action: program_next }     # Shift + Up: the next program
      down: { action: program_prev }
```

- **The toggle (or hold) button** has no action of its own (leave `action:` out): it only toggles the mode, and presets can't rebind it. A modifier's button is the exception (above). A `button.action` set for it from the host is stored but not used. Its presses still show as EVENTs. Its LED is lit while the mode is active (the default `led:`).
- **While a mode is active,** unbound keys are silent, and the buttons keep their normal actions. A mode with no `keys:` leaves the keyboard playing.
- **Buttons.** `buttons:` maps button ids to the same actions as keys (not sub-modes or ranges). While the mode is active, a bound button runs that instead of its own action; its release sends nothing, even if the mode has ended by then. A button pressed before the mode started is released normally. A mode's own toggle/hold button can't be bound.
- **Key actions:** `preset_select` (`preset: N`), `preset_next`, `preset_prev` (these need 2 or more preset slots), `octave_down`, `octave_up`, `octave_reset`, `transpose_down`, `transpose_up`, `transpose_reset`, `channel` (`channel: 1..16`), `channel_up`, `channel_down` (16 wraps to 1), `velocity_curve` (`curve: normal | soft | hard | fixed`, velocity matrices only), and `{ mode: <id> }` to go to a sub-mode.
- **Ranges.** `keys:` may also hold one range, `white_from: <key>` (white keys only) or `from: <key>` (every key), with `action: preset_select` or `channel` and an optional `count:` (default: every slot, or 16 channels). The keys are numbered 1, 2, ... in order. Single keys can sit beside a range in the same mapping.
- **What it sends.** A preset switch from a mode is the same as from a button: Note Offs, PRESET_CHANGED and PARAM_CHANGED `preset.active`, and the preset's Program Change. Octave/transpose keys send PARAM_CHANGED like the octave buttons. `channel`, `channel_up`, `channel_down` and `velocity_curve` send PARAM_CHANGED and make the preset dirty, like a host edit: COMMIT keeps the change, a preset switch drops it. A display shows the preset's name or `CH <n>` ([core/README.md "Displays"](../../core/README.md#displays)).
- **`layer.enabled`** (one per mode, in the "Layers" group, global) is there on a device with layer twins or a cycle. 0: the mode can't be entered (its button is as if it weren't there, so a `with:` press is the button's own action; a mode that is on can still be left), and a cycle skips that step. Defaults are true, a step's `enabled:` aside.
- **The builder checks** that ids are unique (mode, cycle and step ids together), that every mode has exactly one of `toggle:`, `hold:`, `parent:` or `cycle:`, that bound buttons exist and don't enter a mode, that parents exist and don't loop, that every sub-mode is reachable from a key, that `keys:` and sub-modes have a key matrix to use, that keys are on the keyboard and bound once per mode, and the actions' numbers.

**Generated code** (`gm_device.h` `gm_mode_t`, only with `modes:`): `GM_N_MODES` in `gm_device_config.h` (0 without modes: the mode code is then left out; without a key matrix, `GM_HAS_MATRIX 0`, the key half of it is: the key table lookups and the Note Offs for held keys), and `gm_modes[GM_N_MODES]`, with one key table per mode sorted by key, `{ key index, GM_MK_* op, arg }`, and one button table per mode with `buttons:`, `{ button index, op, arg }` (`hold`, `n_buttons`, `buttons` are the struct's last fields, so a config generated before them still builds), then `with`, `cycle`, `colour`, `name` ("Cycles"; `name` is the step's name, else the mode's id, the label of `layer.enabled`) and `modifier` (1: a modifier). `GM_MODES_ENABLED` lists the `layer.enabled` defaults. The runtime keeps 2 bytes for the active and held modes, 8 for the buttons pressed in a mode and 6 for the preset blink; with layers or cycles 24 more (the layer-only toggles, what EVENT 8 last said) and one per cycle.

## Button layers

A root mode (`toggle:` or `hold:`) can give buttons a second meaning while it is active: a **layer**. Each button in `layer:` gets a **twin**, a button of its own on the same switch, with its own action, CC or note, toggle state, DAW role and LED look, and the button's LED pins.

```yaml
buttons:
  - { id: b2, pin: ..., action: cc_toggle, cc: 86, led: b2_led, daw: mute_1 }
leds:
  - { id: b2_led, pin: ..., pin2: ..., pin3: ..., idle: white, active: blue }
modes:
  - id: arm
    toggle: { button: button_mode, with: shift }
    led: button_mode_led
    layer:
      b2: { cc: 52, daw: arm_1, active: red }   # B2 in the arm layer: CC 52, red while on
```

- **What a twin leaves out is the button's:** `action:` (so `cc:` alone keeps a `cc_toggle` a toggle), `cc:` / `note:` / `preset:`, and the LED looks. `name:` defaults to "<button name> <mode id>" ("B2 arm"). `daw:` is the twin's own, or none.
- **One live at a time.** While the mode is active the twin takes the button's presses and drives the LED; otherwise the button does. A press's release goes to whichever took the press, even if the layer changed in between. When layers that are on at once have twins of the same component, the first of these wins: the held hold mode, the toggled key mode, a cycle's current step, then layer-only toggles; between steps of different cycles, or between layer-only toggles, the one earlier in `modes:`.
- **Each keeps its state.** Toggling B2 in the arm layer leaves its mute state alone; switching layers redraws the LED with the live one's state.
- **They are ordinary components:** the twin is the next button after the YAML's (id `<button>_<mode>`, e.g. `b2_arm`) and its LED the next LED (`b2_led_arm`). A host's echo of the twin's CC sets its toggle state, and GET_COMPONENT lists them with their roles, so DAW scripts see `arm_1` next to `mute_1` and drive each LED as usual. Their `layer` byte (GET_COMPONENT, kinds 1, 4, 5 and 7) is the mode index + 1 (0 for the YAML's own components): a twin is live only while EVENT 8 says its mode is on.
- **Their settings are global**, the same in every preset, stored once with the global settings: `layer.button.action` and `layer.button.number` (one per button twin, labelled with the twin's name), `layer.encoder.cc` and `layer.encoder.mode` (one per encoder twin), in the config's "Layers" group. `button.*` and `encoder.*` cover only the YAML's own buttons and encoders, and stay per preset; presets: can't set a twin. A preset switch leaves the twins' settings alone (and drops toggle states, as for every button).
- **EVENT 8 (layer)**, while `debug.events` is 1: index = mode index, value 1 when a mode's layer turns on (a toggle, a hold mode's button, a cycle step), 0 when it turns off; the offs of one change come first. Every time the host sets `debug.events` to 1 (also when it already is), the device sends EVENT 8 value 1 for each mode that is on, so a script learns the state. Modes are reported whether or not they have twins (a cycle step without a layer too); sub-modes aren't.
- **The builder checks** that layers are on root modes, that a button isn't both in the mode's `buttons:` and its `layer:`, that plain `toggle:` and `hold:` buttons, modifiers and the `with:` buttons have no twin, that a modifier's layer has no twin of a button it already toggles a mode with, and that a twin isn't given a twin.
- **A `toggle: { button, with }` button may have a twin**, in its own mode's layer or another's: Shift + the twin toggles the mode just as Shift + the button does (the mode's toggle is the button or any of its twins), and the twin alone is its own action. The Code 61's Pad octave - is the `pads` mode's toggle and, in that layer, `grid_left`.

**Pads and encoders** can be in a layer too, by id, next to buttons:

```yaml
modes:
  - id: pads
    toggle: { button: pad_octave_down, with: shift }
    layer:
      pad_13: { daw: clip_1_1, idle: off }    # a pad twin: name, daw, idle, active
      pad_octave_down: { daw: grid_left }
  - id: pan
    toggle: { button: encoder_mode, with: shift }
    led: encoder_mode_led
    layer:
      encoder_1: { cc: 41, daw: pan_1 }       # an encoder twin: name, cc, daw
```

- **A pad twin** plays no note. While its layer is active, the physical pad's hits are the twin's: an EVENT type 7 with the twin's pad index and the peak (`debug.events` on), 0 on release, and its LED is the twin LED: the host's state (FB_LED, FB_LED_RGB), lit with its active look while the twin's hit is held. The pad's own note, LED and state stay with the pad; sensing stays per physical pad (no extra ADC reads or pins), and a hit's release goes to whichever took the hit. Twins come after the YAML's pads (GET_COMPONENT kind 7, id `<pad>_<mode>`, its LED `<pad LED>_<mode>`); they have no `pad.note`. Keys: `name`, `daw`, `idle`, `active`.
- **An encoder twin** is the next encoder after the YAML's (id `<encoder>_<mode>`), with its own `layer.encoder.cc` (`cc:`, else the encoder's) and `layer.encoder.mode` (it starts as absolute). `cc: none` (`layer.encoder.cc` -1) sends no CC at all, only its EVENTs (type 4, `debug.events` on): for a DAW script's knob. The encoder is decoded once, in the scan; while the layer is active its turns go to the twin: its CC, mode, absolute value and EVENT index, and the encoder's own value is kept for when it comes back. An encoder with `ring:` / `ring_center:` can't have a twin yet. Keys: `name`, `cc`, `daw`.
- **Cost:** the twins' params are globals: they count once in the store's worst case, not per preset slot (the Code 61's 24 encoder and 14 button twins and 7 modes: 128 B of the global record).

**Generated code:** `gm_button_t` / `gm_led_t` / `gm_encoder_t` `.layer` (mode index + 1; 0 = not a twin) and `.base` (the button / LED / encoder it is a twin of), `gm_pad_twins[]` (`gm_pad_twin_t`: layer, base pad, LED), `gm_mode_t.with` (button index + 1, 0 = none), and in `gm_device_config.h` `GM_LAYERS` (the number of twins), `GM_PAD_TWINS`, `GM_BTN_TWINS` (the last `GM_BTN_TWINS` buttons are twins) and `GM_ENC_TWINS` (the last `GM_ENC_TWINS` encoders are twins and aren't scanned); the twins' values are the tails of `gm_cfg`'s button and encoder arrays, under the `layer.*` params. Without twins or cycles the layer code is left out.

## Cycles

A **cycle** is a group of root modes, its **steps**, taken in turn by one button: exactly one step is on at a time, and each press of the button moves to the next enabled one. Each step is a mode of its own, with its own layer.

```yaml
modes:
  - id: encoders
    cycle: { button: encoder_mode, with: shift }   # Shift + Encoder mode: the next enabled step
    led: encoder_mode_led                            # shows the step's colour
    steps:
      - { id: enc_midi, name: MIDI, colour: blue }   # no layer: the components as they are
      - id: enc_zyn
        name: ZYN
        colour: cyan
        layer:
          encoder_1: { cc: none, daw: zynthian.zynpot_1 }
          bank_left: { daw: device_bank_prev }
      - { id: enc_pan, name: PAN, colour: red, enabled: false, layer: { encoder_1: { cc: none, daw: pan_1 } } }
```

- **`cycle: { button, with }`** (`with:` optional; or `cycle: <button id>`) instead of `toggle:` / `hold:` / `parent:`. Each press (with `with:` held, if given) moves to the next enabled step, round the end, skipping disabled ones; with only one enabled, the press shows its name again. With `with:`, a plain press is the button's own action, as for `toggle:`; without it, the button has no action of its own.
- **Steps** (2 to 8): `id`, `name` (up to 16 characters; the display shows it, as much as fits, when the step is switched to; default: the id), `colour` (an LED look for the cycle's LED, resolved for its kind like `led_defaults:`, or a full colour, `"#ff8000"` or `blue 20%`: as it is on an LED that can show any colour, the nearest on/off colour elsewhere; default: its active look), `enabled` (default true) and `layer:` (as "Button layers"). No `keys:` or `buttons:`. A step is a root mode with its own mode index, in `modes:` order where the cycle is, so its twins' `layer` is that index + 1.
- **The LED** (`led:`, default the button's LED) shows the current step's colour all the time, over a host's level.
- **Power-on** starts each cycle at its first enabled step; the step isn't remembered across power-off. A preset switch doesn't change it.
- **Disabled steps** (`layer.enabled` 0, "Modes"; the YAML's `enabled:` is its default) are skipped. With every step of a cycle disabled the first step counts as enabled. Changing `layer.enabled` takes effect at the next press.
- **Switching** runs the layers' update, the display shows the new step's name, and EVENT 8 says 0 for the old step and 1 for the new one.
- **Independent** of every other mode: a cycle, layer-only toggles and a key mode can all be on together ("Modes").
- **The builder checks** that a cycle has 2 to 8 steps, that step ids are unique among all modes, that its button doesn't enter another mode (toggle, hold or cycle), that `colour` is a look of the cycle's LED (a look another kind of LED has is a warning: the nearest it can show), that the cycle has an LED for its colours, and that at least one step is enabled.

**Generated code:** each step is a `gm_modes[]` row with the cycle's button as `toggle` / `with`, `cycle` = the cycle's number + 1, `colour` = 0x80 | the LED pins to light (bit 0 pin, 1 pin2, 2 pin3; 0 = the active look) and `name`; `GM_N_CYCLES` in `gm_device_config.h`. A step with a full colour on an LED that can show it also has `gm_mode_rgb[]` (`GM_LED_RGB(0xRRGGBB)`, 0 for the others; `GM_MODE_RGB 1`), and `colour` is its nearest on/off colour. The runtime keeps one byte per cycle (its current step).

## How it runs

The modes have no loop of their own. The other controls hand them their input:
- **Buttons** (`buttons_sample`): a mode's toggle, hold or cycle button goes to `mode_toggle_button`, and a button the active mode binds goes to `mode_function_button`. A hold mode whose button was held through a preset switch still ends with it (`mode_late_release`).
- **Keys** (the key matrix's `keys_poll`): while a mode with `keys:` is on (`keys_mode`), a key runs its binding (`mode_key`) instead of playing.
- **LEDs** (`leds_update`): `mode_led` says what a mode's LEDs show: lit or blinking while it's on, a cycle step's colour, and the preset number's blink after a preset switch from a mode.

The key and button modes share one current mode (`mode_cur`), and a held hold mode is `hold_cur`. With layers or cycles (`GM_LAYER_PARAMS`), each layer-only toggle and each cycle step can be on at the same time as others. Whenever a mode goes on or off, `layers_update` works out the live one of each control and its layer twins (the strongest layer wins, `layer_rank`), and sends EVENT 8 for the modes that changed.

`gm_init` calls `modes_init` before the first `layers_update`, and `cycles_start` once the settings are loaded. `gm_layers_report` (from the protocol) asks for an EVENT 8 replay, which `gm_poll` sends (`layers_report_poll`). On a device without modes, `controller.c` has a `gm_layers_report` that does nothing.

## What it shares

Through `core/include/gm_controls.h`:
- the functions above, for the buttons, the key matrix and the LEDs;
- `layers_update` writes each control's live twin: `btn_live` (buttons), `enc_live` (encoders), `pad_live` (pads), and `led_dead` (LEDs), asking the LEDs to draw an LED that changed hands again (`led_redraw`).

It reads the buttons' `btn_held`, and uses their `step_and_show` for its octave and transpose bindings.

## Tests

`core/test/test_modes.c`, with `core/test/modes-test.yaml`; `test_controller.c` (the Keystation's Edit mode: keys bound with `keys:`, its sub-modes), `test_matrix.c` (`matrix-test.yaml`: a hold mode, a layer and a cycle on switch-matrix buttons), `test_encoders.c` (encoder twins in a cycle) and `test_pads.c` (pad twins).
