# encoders

A control ([core/README.md "Controls"](../../core/README.md#controls)): rotary encoders on two pins each (MCU pins, a hub's, or a switch matrix's), sending absolute or relative values, with acceleration. Its YAML (`encoders:`, `encoder_settings:`): the fields in [core/README.md "Controls"](../../core/README.md#controls), "Encoder modes" and "Encoder acceleration" below, and the message kinds in [core/README.md "MIDI messages"](../../core/README.md#midi-messages). An encoder's LED ring is the LEDs' ([leds](../leds/README.md)).

- `encoders.c`: counting the detents and sending them. Built when the device has encoders.
- `__init__.py`: the builder part: `validate` (its YAML: pins, mode, `mcu:`, rings, acceleration and `encoder_settings:`), `c_defines` (`GM_ENC_ACCEL`) and `c_tables` (`gm_encoder_t encoders[]`, `gm_enc_accel[]`), and when it's built.

## Encoder modes

`encoder.mode` (YAML `mode:`): 0 absolute (`absolute`: 0..127, sends the value), 1 relative two's complement (`relative_twos`: 1 = +1, 127 = −1), 2 relative offset (`relative_offset`: 65 = +1, 63 = −1), 3 relative sign-magnitude (`relative_sign`: 1 = +1, 65 = −1), and with the message kinds ([core/README.md "MIDI messages"](../../core/README.md#midi-messages)) the absolute 4 `cc14`, 5 `nrpn`, 6 `nrpn14`, 7 `rpn`, 8 `rpn14`.

## Encoder acceleration

Off by default: one step per detent, however fast the knob turns. With `acceleration:`, a quick turn moves more than one step per detent, so a flick sweeps the range and a slow turn stays exact.

```yaml
encoder_settings: { acceleration: medium }   # every encoder that doesn't set its own (default: off)
encoders:
  - { id: encoder_1, pin_a: PB6, pin_b: PB7, cc: 20 }                        # medium, from encoder_settings
  - { id: encoder_2, pin_a: PB8, pin_b: PB9, cc: 21, acceleration: off }     # always 1 per detent
  - { id: encoder_3, pin_a: PB10, pin_b: PB11, cc: 22,
      acceleration: { slow: 100ms, fast: 8ms, max: 16, curve: quadratic } }
```

- **The curve.** The main loop times each turn: the time per detent since the encoder last moved, smoothed (each turn moves the average half way to the new time). From `slow` (or slower) down to `fast` (or faster), x goes from 0 to 1, and each detent moves 1 + (max − 1) · x steps (`linear`), x² (`quadratic`) or x³ (`cubic`). Steps are counted in 1/16 and the fraction is carried to the next detent, so a speed between gives e.g. 2, 3, 2, 3 for ×2.5.
- **Precise where it matters.** A detent `slow` or more after the last one moves 1 step and starts the average again from slow, so slow turns are exactly 1 per detent. Turning back does the same and drops the carried fraction: the first detent back is 1 step. A detent never moves less than 1 step.
- **What it applies to:** the CC in every mode (absolute values clamped to 0..127, relative values to −64..63 per message, as before), the Mackie Control V-Pot (at most 15 per message) and the EVENT a DAW script reads (type 4): all get the same accelerated steps. Twins (layers, cycle steps) share their physical encoder's acceleration and speed; a mode switch in mid-turn carries on at speed. Shift + a cycle's button still moves one step per press: buttons aren't accelerated.
- **Presets** (`off`, `low`, `medium`, `high`) and what they do on a 24-detent encoder (detents → steps):

| | slow | fast | max | curve | a quick full turn (24 detents, 8 ms apart) | half a turn quickly (12, 8 ms) | a brisk turn (24, 20 ms) | a steady turn (24, 40 ms) |
|---|---|---|---|---|---|---|---|---|
| `low` | 60ms | 15ms | 4 | linear | 91 | 43 | 81 | 52 |
| `medium` | 80ms | 10ms | 8 | quadratic | 174 | 78 | 132 | 71 |
| `high` | 100ms | 8ms | 16 | quadratic | 343 | 151 | 263 | 157 |

  (absolute values stop at 127). A mapping takes `slow` (2ms..1000ms), `fast` (1ms, shorter than slow), `max` (1..32; 1 is the same as off) and `curve` (`linear`, `quadratic`, `cubic`); keys left out come from `encoder_settings:`'s acceleration, else from `medium`. Bare `off` works with or without quotes.
- **Build time only, for now:** the settings are in the device tables (flash), not params, so a preset or the web app can't change them yet; a preset's `acceleration:` and a twin's are refused with where to set it.
- **Generated:** `GM_ENC_ACCEL` in `gm_device_config.h`; with 1, `gm_enc_accel[]` (`gm_enc_accel_t`: slow ms, fast ms, max, curve) has one entry per scanned encoder (off: `{ 0, 0, 1, GM_ACCEL_LINEAR }`). With every encoder off it is 0 and the code compiles to nothing (the same image as before acceleration). RAM with it: 8 bytes per scanned encoder; the divides run only when an encoder moves.

## How it runs

Encoders have two halves:
- **Counting**, from the scan. `gm_scan_tick` (the scan interrupt) calls `encoder_step` for each encoder on every tick. With the hardware scan, an encoder on a scanned switch matrix is stepped from its frames instead. `encoder_step` reads A and B (one load each when they're MCU pins, `vpin_in_reg`), and counts a detent every `steps_per_detent` quarter steps. An encoder on an I2C hub is stepped from the main loop, since I2C can't run in the interrupt.
- **Sending**, from the main loop. `encoders_poll` takes the detents counted since the last pass and sends them, in the encoder's `mode`, to the live one of it and its layer twins. Acceleration scales the steps by how fast it turns.

The count is free running: only `encoder_step` writes it (`encoders_poll`, and the raw readings in `monitor.c`, read it), so a detent counted between the main loop's load and store isn't lost. Twins aren't scanned: only the scanned encoders (`NE_HW`) have counts.

## What it shares

Through `core/include/gm_controls.h`:
- `encoder_step` and `enc_i2c`, for the scan tick;
- `enc_count`, the detent counts, for the raw readings (`GM_MONITOR`);
- `enc_value` and `enc_top`, `enc_span`, `enc_out`, `enc_at`, which host feedback uses to take a host's value;
- `enc_live`, the live twin of each encoder, which the modes (`layers_update`) write.

Host feedback calls `gm_encoder_host` (`gm_feedback.h`). On a device without encoders, `controller.c` has a stub that does nothing.

## Tests

`core/test/test_encoders.c`, with `core/test/encoders-test.yaml`.
