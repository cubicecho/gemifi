# modes

A control ([core/README.md "Controls"](../../core/README.md#controls)): modes that change what keys and buttons do while they're on, entered with a toggle, hold or cycle button. Its YAML (`modes:`) and what each kind does: [core/README.md "Modes"](../../core/README.md#modes), "Button layers" and "Cycles".

- `modes.c`: which modes are on, their key and button bindings, the layers and cycles, and the LEDs that show them. Built when the device has modes.
- `__init__.py`: when it's built. The YAML is still validated in `tools/builder/gemifi_device.py` (`validate_modes`).

## How it runs

The modes have no loop of their own. The other controls hand them their input:
- **Buttons** (`buttons_sample`): a mode's toggle, hold or cycle button goes to `mode_button`, and a button the active mode binds goes to `mode_btn`. A hold mode whose button was held through a preset switch still ends with it (`mode_late_release`).
- **Keys** (the key matrix's `keys_poll`): while a mode with `keys:` is on (`keys_mode`), a key runs its binding (`mode_key`) instead of playing.
- **LEDs** (`leds_update`): `mode_led` says what a mode's LEDs show: lit or blinking while it's on, a cycle step's colour, and the preset number's blink after a preset switch from a mode.

The key and button modes share one current mode (`mode_cur`), and a held hold mode is `hold_cur`. With layers or cycles (`GM_LAYER_PARAMS`), each layer-only toggle and each cycle step can be on at the same time as others. Whenever a mode goes on or off, `layers_update` works out the live one of each control and its layer twins (the strongest layer wins, `layer_rank`), and sends EVENT 8 for the modes that changed.

`gm_init` calls `modes_init`, and `cycles_init` and `cycles_start` around loading the settings. `gm_layers_report` (from the protocol) asks for an EVENT 8 replay, which `gm_poll` sends (`layers_report_poll`). On a device without modes, `controller.c` has a `gm_layers_report` that does nothing.

## What it shares

Through `core/include/gm_controls.h`:
- the functions above, for the buttons, the key matrix and the LEDs;
- `layers_update` writes each control's live twin: `btn_live` (buttons), `enc_live` (encoders), `pad_live` (pads), and `led_dead` and `led_shown` (LEDs).

It reads the buttons' `btn_held`, and uses their `shift` for its octave and transpose bindings.

## Tests

`core/test/test_modes.c`, with `core/test/modes-test.yaml`; `test_encoders.c` (encoder twins in a cycle) and `test_pads.c` (pad twins).
