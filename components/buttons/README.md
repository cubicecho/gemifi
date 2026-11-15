# buttons

A control ([core/README.md "Controls"](../../core/README.md#controls)): buttons and switches on any input pin (an MCU pin, a hub's, a switch matrix's), each with an action. Its YAML (`buttons:`) and the actions: [core/README.md "Controls"](../../core/README.md#controls) ("Button actions"), "MIDI messages", "Modes" and "Button layers".

- `buttons.c`: sampling and debouncing the buttons, their actions, the CC ramps and the pitch-bend buttons' glide. Built when the device has buttons.
- `__init__.py`: when it's built. The YAML is still validated in `tools/builder/gemifi_device.py`.

## How it runs

The main loop (`gm_poll`) calls:
- `buttons_sample` every `BTN_SAMPLE_MS` (1 ms). A press counts at the first sample that read it closed in every scan since the sample before, so a quick tap is never lost. A release counts once no scan has read it closed for `BTN_STABLE` (16) samples in a row, which rides out contact bounce.
- `ramps_ms` once per elapsed millisecond: the `cc_ramp` buttons glide towards 127 or 0, and the pitch-bend buttons' bend towards its target.

Each press or release goes, in this order:
1. to the safety combos (`gm_safety_button`);
2. to the live one of a button and its layer twins;
3. to the modes: a mode's toggle, hold or cycle button (`mode_button`), or a function button of the active mode (`mode_btn`);
4. to the button's own action (`button_changed`).

A preset switch (`gm_release_all`) calls `buttons_off`, which lets go of every note and CC the buttons hold. Buttons held through it release silently.

## What it shares

Through `core/include/gm_controls.h`:
- `btn_held`, `btn_toggled` and `ramp_q8`, which the LEDs show (host feedback also sets `btn_toggled`);
- `btn_stable`, for `gm_poll`, the LED walk and the idle time (`gm_idle_ms`);
- `btn_live`, the buttons that take a press, which the modes (`layers_update`) write;
- `btn_hist`, the samples, for the raw readings (`GM_MONITOR`);
- `shift`, which the modes' octave and transpose bindings use, and `BTN_CH`.

## Tests

In `core/test/`: `test_controller.c` (debounce, actions, ramps, presets), `test_modes.c` (modes, layers, cycles) and `test_msg.c` (program, MMC, SysEx and panic actions).
