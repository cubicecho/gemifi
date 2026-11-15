# encoders

A control ([core/README.md "Controls"](../../core/README.md#controls)): rotary encoders on two pins each (MCU pins, a hub's, or a switch matrix's), sending absolute or relative values, with acceleration. Its YAML (`encoders:`, `encoder_settings:`) and what each setting does: [core/README.md "Controls"](../../core/README.md#controls), "Encoder acceleration" and "MIDI messages". An encoder's LED ring is the LEDs' ([leds](../leds/README.md)).

- `encoders.c`: counting the detents and sending them. Built when the device has encoders.
- `__init__.py`: when it's built. The YAML is still validated in `tools/builder/gemifi_device.py`.

## How it runs

Encoders have two halves:
- **Counting**, from the scan. `gm_scan_tick` (the scan interrupt) calls `encoder_step` for each encoder on every tick. With the hardware scan, an encoder on a scanned switch matrix is stepped from its frames instead. `encoder_step` reads A and B (one load each when they're MCU pins, `vpin_in_reg`), and counts a detent every `steps_per_detent` quarter steps. An encoder on an I2C hub is stepped from the main loop, since I2C can't run in the interrupt.
- **Sending**, from the main loop. `encoders_poll` takes the detents counted since the last pass and sends them, in the encoder's `mode`, to the live one of it and its layer twins. Acceleration scales the steps by how fast it turns.

The count is free running: only `encoder_step` writes it and only `encoders_poll` reads it, so a detent counted between the main loop's load and store isn't lost. Twins aren't scanned: only the scanned encoders (`NE_HW`) have counts.

## What it shares

Through `core/include/gm_controls.h`:
- `encoder_step` and `enc_i2c`, for the scan tick;
- `enc_count`, the detent counts, for the raw readings (`GM_MONITOR`);
- `enc_value` and `enc_top`, `enc_span`, `enc_out`, `enc_at`, which host feedback uses to take a host's value;
- `enc_live`, the live twin of each encoder, which the modes (`layers_update`) write.

Host feedback calls `gm_encoder_host` (`gm_feedback.h`). On a device without encoders, `controller.c` has a stub that does nothing.

## Tests

`core/test/test_encoders.c`, with `core/test/encoders-test.yaml`.
