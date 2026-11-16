# pads

A control ([core/README.md "Controls"](../../core/README.md#controls)): velocity pads, read from an ADC pin or a mux channel, that play a note with a velocity from how hard they're hit.

```yaml
pads:
  - { id: pad_1, pin: { cd74hc4051: pad_mux_b, number: 4 }, note: 36, led: pad_1_led }
  - { id: pad_2, pin: { cd74hc4051: pad_mux_b, number: 5 }, note: 37, led: pad_2_led }
pad_settings:
  channel: 10
```

Its YAML (`pads:`, `pad_settings:`): "YAML" below.

## YAML

Pressure pads (FSR style, like drum pads) play a note with a velocity from how hard they are hit. Each pad is one analog source, an ADC pin or a mux channel, and the list order is the pad order (index labels, `pad.note[i]`, EVENT index). What all pads share is in `pad_settings:`.

```yaml
cd74hc4051:
  - id: pad_mux
    pin_s0: { number: PC0, allow_other_uses: true }   # select lines shared with another mux
    pin_s1: { number: PC1, allow_other_uses: true }
    pin_s2: { number: PC2, allow_other_uses: true }
    common_pin: PA0
    delay: 10us                # use the same delay on muxes that share select lines
pads:
  - { id: kick,  pin: { cd74hc4051: pad_mux, number: 0 }, note: 36 }
  - { id: snare, pin: { cd74hc4051: pad_mux, number: 1 }, note: 38 }
  - { name: Hat, pin: { number: PA2, inverted: true }, note: 42 }   # pressure lowers this one's reading
pad_settings:                  # all optional; these are the param defaults
  channel: 10                  # 1..16, or 0 (the default): midi.channel   (pad.channel, per preset)
  curve: normal                # normal | soft | hard | fixed              (pad.curve, per preset)
  aftertouch: false            # poly pressure while held                 (pad.aftertouch, per preset)
  # octave: 0                  # presets only: -4..4, the notes + 12 each  (pad.octave, per preset)
  threshold: 200               # reading that starts a hit, 1..4095       (pad.threshold, global)
  release: 100                 # the pad is let go below this             (pad.release, global)
  full: 3000                   # peak reading for velocity 127            (pad.full, global)
  window: 3ms                  # 1..50ms: peak search after the threshold (pad.window, global)
  retrigger: 20ms              # 0..500ms: no new hit after a release     (pad.retrigger, global)
```

- **Sampling.** Every pad is read once a millisecond from the main loop, in the same context as the analog inputs: they often share a mux's select lines and the ADC, so reading pads from the scan interrupt would race with the analog reads. Pads on muxes are read sorted by channel, so muxes with the same select lines switch them once per channel (the Code 61: 8 switches for 16 pads). On an STM32F1 a 16-pad scan takes about 0.15 ms (16 conversions of ~6 us plus 8 settles of 10 us).
- **A hit.** Readings are 0..4095 (after `inverted`). When a pad reaches `pad.threshold`, its peak over the next `pad.window` ms is taken (4 readings at 3 ms) and Note On goes out with velocity 1..127, linear from `threshold` to `full`, then shaped by `pad.curve` like the keys' curves (soft, hard; fixed = `velocity.fixed`). A pad let go within the window plays at once with the peak so far. It is released (Note Off) below `pad.release`, and can't start a new hit for `pad.retrigger` ms after that (bounce, double hits). The thresholds are live params: tune them from the web app. A soft hit (a peak under twice `pad.threshold`) that falls below `pad.release` again within 3 ms is noise, not a hit: pads see such spikes while a pad board's LEDs change.
- **Pad octave** (`pad.octave`, -4..4, per preset, starts at 0): every pad's note + 12 × it; a note that would go past 0..127 plays in the nearest octave in range. The `pad_octave_down` / `pad_octave_up` button actions move it, and the display shows `PAD +1`. A hit held across a change gets the Note Off of the note it played.
- **Polyphonic aftertouch** (`pad.aftertouch`): while a pad is held, its reading from `release` to `full` is sent as poly pressure 0..127, when it moves by 2 or more, at most every 10 ms per pad, and 0 before the Note Off.
- **At start-up** a pad must read below `pad.release` once before it can play, so a pad held at power-up, or one with the wrong polarity (`inverted:`), stays silent. Such pads are named in a WARN log at the boot log (about 1.5 s).
- **EVENTs** (`debug.events`): a pad's own hits only play notes. A pad's layer twin ([modes "Button layers"](../modes/README.md#button-layers)) sends EVENT type 7 instead: its pad component index (the pads, then the twins), and on a hit the peak reading × 4 (1..16380), 0 on release. That is also the raw number to set `threshold` and `full` from: hit the pads with their layer on.
- **Presets** may set `pads: { <id>: { note: N } }` and `pad_settings: { channel, curve, aftertouch }`; the sensing (`threshold`, `release`, `full`, `window`, `retrigger`) is the pads' calibration and the same in every preset.
- **Generated code** (`gm_device.h` `gm_pad_t`, only with `pads:`): `GM_N_PADS` and the `GM_PAD_*_DEFAULT`s in `gm_device_config.h`, `gm_pads[GM_N_PADS]` (pin, name, LED) and `gm_pad_note_defaults[]` in `gm_device_config.c`; with pad twins also `GM_PAD_TWINS` and `gm_pad_twins[]` (`gm_pad_twin_t`: layer, base pad, LED). A device without pads has no pad params, no "Pads" group and no pad code. RAM: about 16 bytes and one param value per pad, plus 8 values.

## Files

- `pads.c`: reading the pads and playing them. Built when the device has pads.
- `__init__.py`: the builder part: `validate` (its YAML), `c_defines` and `c_tables` (its generated C: `GM_N_PADS`, the `GM_PAD_*_DEFAULT`s, `gm_pads[]`, `gm_pad_twins[]`), and when it's built.

## How it runs

The main loop (`gm_poll`) calls `pads_scan` every millisecond ("Sampling" above).

Each pad goes through four states: blocked (at start-up, until it reads below `pad.release`), idle, peak (the `pad.window` after the threshold) and on. A preset switch (`gm_release_all`) calls `pads_off`, which sends Note Off for every sounding pad and blocks held pads until they're let go.

## What it shares

Through `core/include/gm_controls.h`:
- `pad_state` and `pad_lit`, which the LEDs show, and `pad_owner`, which of the pad and its twins took the last hit, so whose LED lights;
- `pad_live`, the live layer twin of each pad, which the modes (`layers_update`) write;
- `pad_raw`, the last readings, for the raw readings (`GM_MONITOR`).

It sends with the controller's `midi3`, `midi_chan` and `send_event`.

## Tests

`core/test/test_pads.c`, with `core/test/pads-test.yaml` and, on the hardware analog scan, `core/test/pads-hw-test.yaml`.
