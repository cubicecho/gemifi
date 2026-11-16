# analog

A control ([core/README.md "Controls"](../../core/README.md#controls)): analog inputs, such as pots, faders, wheels and pressure strips, read from an ADC pin, a mux channel or an XY pad axis.

```yaml
analog:
  - { id: pot_1, pin: PA0, cc: 7 }
  - { id: pot_2, pin: PA1, cc: 10 }
```

<!-- keys -->

## Pitch wheels and aftertouch

An analog input with `analog.mode` = pitch bend (YAML `mode: pitch_bend`, or set from the web app) sends 14-bit pitch bend on `midi.channel` instead of a CC; with `analog.mode` = channel pressure (`mode: channel_pressure`) it sends channel pressure (aftertouch) 0..127, `adc_min` = released, `adc_max` = pressed hard, with the CC's hysteresis.

```yaml
analog:
  - { id: pitch_wheel, name: Pitch, pin: PA4, mode: pitch_bend }
  - { id: mod_wheel, name: Mod, pin: PA5, cc: 1 }     # a mod wheel is a plain CC 1
```

- **Centre.** The input's first reading after power-up is the centre, because a sprung wheel rests there; keep it untouched while the device starts. 16 raw ADC counts either side of it send no bend, so the wheel always returns to exactly 8192.
- **Range.** From the centre to each end (`adc_min` / `adc_max`, less 8 counts) is one half of 0..16383, so the bend is symmetric even when the centre isn't in the middle of the travel.
- **Noise.** Changes under 6 raw counts are dropped, except to the centre or an end. Both are raw counts, so a wheel that only moves the reading over part of 0..4095 (set `adc_min` / `adc_max` to its ends) is no twitchier.

## Files

- `analog.c`: reading the inputs and sending what they're set to. Built when the device has analog inputs.
- `__init__.py`: the builder part: `validate` (its YAML: pin, mode, `mcu:`, calibration, takeover, touch) and `c_tables` (`gm_analog_t analogs[]`), and when it's built.

## How it runs

The main loop (`gm_poll`) calls `analog_poll`. Without the hardware analog scan, it reads one input every `ANALOG_MS` (2 ms) in turn, plus every pitch-bend and pressure input each time. With the hardware analog scan, the readings are already there, so it takes every input every millisecond.

Each reading is scaled from `adc_min`..`adc_max` to 0..4095 and smoothed over 4 samples. Then it's sent as `analog.mode` says:
- a 7-bit CC (or another 7-bit kind) when it moves by more than 24 counts;
- a 14-bit kind when it moves by half the bend noise step;
- pitch bend, centred on the power-up reading;
- channel pressure.

The input's `channel:`, `min:` and `max:` apply, and pick-up holds it silent until it reaches the host's value. With `touch: movement` it sends the touch EVENTs.

`analog_init` (from `gm_init`) takes the first reading as the start, with no message. `analog_pickup_off` puts every input back in control of its value, after a preset switch or when the host lets go.

## What it shares

Through `core/include/gm_controls.h`:
- `a_raw`, the last readings, for the raw readings (`GM_MONITOR`);
- `an_kind` and the `AN_CH` channel macro, which host feedback uses to match what it receives to an input.

It sends with the controller's MIDI helpers (`midi3`, `midi2`, `send_value`, `ctl_out`). Host feedback calls `gm_analog_host` (`gm_feedback.h`) with the host's value, for pick-up. On a device without analog inputs, `controller.c` has a stub that does nothing.

## Tests

In `core/test/`: `test_controller.c` (the Keystation, a copy of its YAML), `test_msg.c` (the message kinds), `test_feedback.c` (pick-up) and `test_pads.c` (an XY pad's axes).
