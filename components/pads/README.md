# pads

A control ([core/README.md "Controls"](../../core/README.md#controls)): velocity pads, read from an ADC pin or a mux channel, that play a note with a velocity from how hard they're hit. Its YAML (`pads:`, `pad_settings:`) and what each setting does: [core/README.md "Pads"](../../core/README.md#pads).

- `pads.c`: reading the pads and playing them. Built when the device has pads.
- `__init__.py`: when it's built. The YAML is still validated in `tools/builder/gemifi_device.py` (`validate_pads`).

## How it runs

The main loop (`gm_poll`) calls `pads_scan` every millisecond. The pads are read there and not in the scan interrupt, because they share the mux select lines and the ADC with the analog inputs. Pads on muxes are read sorted by channel, so muxes that share select lines switch them once per channel.

Each pad goes through four states: blocked (at start-up, until it reads below `pad.release`), idle, peak (the `pad.window` after the threshold) and on. A preset switch (`gm_release_all`) calls `pads_off`, which sends Note Off for every sounding pad and blocks held pads until they're let go.

## What it shares

Through `core/include/gm_controls.h`:
- `pad_state` and `pad_lit`, which the LEDs show;
- `pad_live`, the live layer twin of each pad, which the modes (`layers_update`) write;
- `pad_raw`, the last readings, for the raw readings (`GM_MONITOR`).

It sends with the controller's `midi3`, `midi_chan` and `send_event`.

## Tests

`core/test/test_pads.c`, with `core/test/pads-test.yaml` and, on the hardware analog scan, `core/test/pads-hw-test.yaml`.
