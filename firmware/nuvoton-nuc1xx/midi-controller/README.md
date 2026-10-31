# midi-controller (NUC1xx)

The generic gemifi MIDI controller firmware for NUC100/NUC120 chips. The hardware comes from a device description (`devices/<device>.yaml`, turned into `gm_device_config.h/.c` by `tools/builder`, format in [`core/README.md`](../../../core/README.md)). The behaviour is the shared core runtime (`core/src/controller.c`), configurable over SysEx from the web updater or `tools/host/gemifi.py`.

> **Status:** running on the Keystation Mini 32 MK3 (2026-10-31), installed through the gemifi bootloader. All 32 keys and velocity, all 7 buttons (sustain CC64, Edit CC80 toggle, octave ±, pitch-bend glide both ways, CC1 ramp), the volume knob (CC7, full range, no drift), all LEDs (incl. the bicolour octave levels) and config get/set/commit checked on hardware; host tests in `core/test` (124 checks; `make test` there).

## Build

```sh
make                                     # Keystation Mini 32: generated from devices/keystation-mini32-mk3.yaml
make DEVICE=<device.yaml>                # another device YAML (generated into build/device/)
make DEVICE_DIR=<config dir> BUILD=<dir> # an already generated config (tools/builder does this)
```

Output: `build/midi-controller.gfw`. With the Keystation config it is 12.7 KB of flash and 1.7 KB of static RAM (of 32 KB / 4 KB).

## What it does (Keystation defaults)

| Control | Default |
|---|---|
| 32 keys | Note On/Off from note 48 (C3), velocity from the time between the two key contacts |
| Sustain | CC 64 momentary, Sustain LED lit while held |
| Edit | CC 80 toggle, Edit LED shows the state |
| Oct − / Oct + | octave −4..+4 (both together: back to 0); LEDs show the octave like the stock firmware (1 = colour A, 2 = both, 3 = colour B, 4 = blinking) |
| Pitch ▼ / ▲ | pitch bend to the end stop with an 80 ms glide, back to centre on release |
| Mod | CC 1 ramp up to 127 over 300 ms while held, back down on release |
| Volume knob | CC 7 |

Every button's action and CC/note number can be changed, and stored with "Save to device" (COMMIT). Holding **Pitch ▼ + Pitch ▲** at power-up starts with the defaults and ignores the stored profile (with the gemifi bootloader in LDROM, the same combo enters the bootloader instead).

At power-up all LEDs light for 200 ms (lamp test). About 1.5 s later the firmware sends `LOG "<name> up"` and its version.

## Config (schema)

Generated from `core/src/settings.c`; arrays are sized by the device. For the Keystation:

| id | name | type | range | default |
|---|---|---|---|---|
| 1 | `midi.channel` | int | 1..16 | 1 |
| 2 | `midi.transpose` | int (semitones) | −24..24 | 0 |
| 3 | `midi.octave` | int | −4..4 | 0 (changed by the octave buttons → PARAM_CHANGED) |
| 4 | `velocity.curve` | enum Normal / Soft / Hard / Fixed | | Normal |
| 5 | `velocity.fixed` | int | 1..127 | 100 (also the velocity of "Note" buttons) |
| 6 | `velocity.fast` | int (0.1 ms) | 1..500 | 12: key time that gives 127 |
| 7 | `velocity.slow` | int (0.1 ms) | 20..2000 | 1200: key time that gives 1 |
| 9 | `button.action[7]` | enum (core/README.md "Button actions") | | CC momentary, CC toggle, Octave down, Octave up, Pitch bend down, Pitch bend up, CC ramp |
| 10 | `button.number[7]` | int | 0..127 | 64, 80, 0, 0, 0, 0, 1 |
| 11 | `bend.glide` | int (ms) | 0..2000 | 80 |
| 12 | `ramp.time` | int (ms) | 0..5000 | 300 |
| 8 | `analog.cc[1]` | int | 0..127 | 7 |
| 13 | `debug.events` | bool | | off (on: EVENT messages for keys, buttons, knob) |
| 14 | `debug.log_level` | enum Off / Errors / Warnings / Info / Debug | | Info |

Devices with encoders also get `encoder.cc` (15) and `encoder.mode` (16).

**Velocity.** Normal is linear in log(time) from 127 at `velocity.fast` to 1 at `velocity.slow`: about −18 per doubling of the key time with the defaults, close to the stock Keystation's feel (stock tables sampled for calibration only). Soft lifts the middle, Hard lowers it, Fixed always sends `velocity.fixed`.

**Profile storage.** Data flash (0x1F000, 4 KB) as a journal: `platform/nuvoton-nuc1xx/src/config_store.c`. The first COMMIT replaces the stock settings journal there (the stock copy is in the device backup).

## How it runs

- **Timer0** interrupt every `scan_us` (250 µs for the Keystation; the stock firmware uses 125 µs, but our scan takes about 100 µs of CPU, too close to that): scans the key matrix and encoders (`gm_scan_tick`). It drives one row high at a time and never two.
- **Main loop:** USB polling, received SysEx, buttons (5 ms debounce), analog inputs (one every 2 ms, 4-sample average, hysteresis), ramps and glides (1 ms), LEDs (10 ms), MIDI out without blocking.
- Pins use the NUC's per-pin data registers (`GPIOx_PDIOn`), so the interrupt and the main loop never overwrite each other's pins.

## Test on hardware (with the stock LDROM, like M1)

1. `tools/swd/nuvoton-nuc1xx/flash-aprom.sh firmware/nuvoton-nuc1xx/midi-controller/build/midi-controller.gfw`
2. Replug USB. All LEDs flash once. `python3 tools/host/gemifi.py hello` should show role app, caps config + log + event.
3. Play keys: `aseqdump` (or the web updater's log) shows notes 48–79 with sensible velocities. Try every button and the knob.
4. `python3 tools/host/gemifi.py get` lists the profile; `gemifi.py set midi.channel=2 --commit`, replug, and check notes arrive on channel 2.
5. `gemifi.py log` while turning on `debug.events` (`gemifi.py set debug.events=1`) shows EVENT lines for keys, buttons and the knob.

If keys are silent: check the row/column wiring assumptions in `research/devices/m-audio-keystation-mini32-mk3/analysis/hw-map.md` §3. Turning on `debug.events` reports key indices, which shows whether the matrix mapping is right. If the octave LED colours look swapped, swap `pin` and `pin2` of those LEDs in the device YAML.
