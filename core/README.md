# gemifi core

Chip-independent C code shared by every gemifi firmware: the SysEx protocol, the config profile, and the generic MIDI controller runtime. A platform (`platform/<family>/`) adds a small HAL; a device YAML (`devices/*.yaml`) describes the hardware; `tools/builder` turns the YAML into C tables and builds.

```
devices/<device>.yaml ──tools/builder──▶ gm_device_config.h / .c ──┐
core/  (protocol, params, controller, virtual pins)                │
components/<name>/  (controls, hubs, screens, buses: those used)   ├─▶ firmware image
platform/<family>/  (HAL: pins, ADC, timer, MIDI transport, storage, reboot, upload)
```

## Files

| File | What |
|---|---|
| `include/gsx.h`, `src/gsx.c` | SysEx frame codec (`protocol/sysex.md` §1–3) |
| `include/crc32.h`, `src/crc32.c` | CRC-32 (zlib's), from a 16-entry table: the settings store, the image header and the uploads, in the firmware and the bootloaders (the ESP uses its own) |
| `include/params.h`, `src/params.c` | param table, GET/SET validation, schema blob (§4.3, §4.4) |
| `include/config_store.h` | stored-profile API; implemented per platform |
| `include/app_proto.h`, `src/app_proto.c` | app-side request handling: HELLO, REBOOT, identity, config, FW_* (via `hal_fw_ops`), LOG/EVENT/PARAM_CHANGED. Where the bootloader takes uploads (nuvoton-nuc1xx, stm32f1: `-DHAL_APP_FW_UPLOAD=0` in their `mk/app.mk`), FW_* and its buffers are left out and every FW_* is answered BAD_STATE; the receive buffer then holds the longest other request (SET_PARAMS with 16 values, 86 bytes of payload) |
| `include/gm_device.h` | the hardware model: virtual pins, hubs, buttons, LEDs, analog inputs, encoders, key matrix, pads |
| `include/gm_hal.h` | what a platform provides |
| `include/gm_controller.h`, `src/controller.c` | the runtime: the scan tick and the hardware scan, MIDI out and the MIDI messages, the preset release, `gm_init` and `gm_poll`; the controls are components |
| `include/gm_controls.h` | what the controller and the control components share: the MIDI helpers, the state one control keeps and another reads (each with its writer), what `gm_poll` and `gm_init` call. Not for the platforms |
| `src/feedback.c` | host feedback (F1): what the host sends lights LEDs, sets toggles, gives analog inputs and encoders its value |
| `src/monitor.c` | the raw readings (MONITOR / RAW, "Raw readings" below): what the controls last read, for a tester |
| `src/midi_in.c` | incoming MIDI: USB-MIDI packets or a byte stream, sorted by port into SysEx (the protocol), feedback and MCU |
| `include/gm_feedback.h` | the feedback switches (`GM_FEEDBACK`, `GM_MCU`, ...) and the calls between the files above |
| `include/gm_mcu.h`, `src/mcu.c` | Mackie Control on the DAW port; empty unless the YAML has `mcu:` |
| `src/vpin.c` | virtual pins: MCU pins via the HAL, hubs through their drivers (`../components/<key>/`), the scan's written-out paths (mux channels, decoder rows, switch matrices, the hardware analog scan), and what the hub base gives its types |
| `../components/<name>/` | the components: hubs, screens, buses and controls, each in a folder with its README ("Components" below); `hub/gm_hub.h` is the hub driver interface |
| `include/gm_pin.h` | a pin (`gm_pin_t`), on the MCU or a hub |
| `src/settings.c` | the param table, sized from the device's component counts |

Rules: C11, no heap, no floating point, no libc beyond `<stdint.h>` (the NUC120 has 4 KB of RAM and 32 KB of flash). Headers are C++-safe (`extern "C"`), for platforms written in C++.

What each component costs (CPU, latency to MIDI out, MIDI traffic, RAM) and the YAML knobs that trade them: [performance.md](performance.md).

## Components

The hardware a device's YAML describes is made of **components**, ESPHome style: one folder each in [`components/`](../components/) at the repo root, holding its builder part (`__init__.py`: its YAML keys, validation and generated C), its C (`<name>.c`, and for most hubs and screens a `<name>.h` with their config), its `README.md` and, for some, its tests (`test/`). The builder finds them by folder (`components/__init__.py`), and that decides what a device compiles (`components.used()`, below). A new hub, strip, driver or display chip is a new folder, without edits to the core; a new control also touches the builder and the controller ("Adding a component").

A **base component** defines what the components on it implement, and holds the code they share. A chip that is one of several for one job is a **platform** of a base: one YAML list for all of them, with `platform:` on each item, as ESPHome's `display:` is. Other chips have a list of their own, named like the ESPHome component. A **control** (buttons, LEDs, the key matrix ...) is a component too: what the player plays, built when the device has it ("Controls" below).

| Component | YAML | What it is |
|---|---|---|
| [hub](../components/hub/README.md) | – | the base of every hub: the driver interface (`gm_hub_ops_t`), the hub state, the hub RAM. Every device builds it |
| [i2c](../components/i2c/README.md) | `i2c:` | the I2C buses (one, or a list with ids); I2C components pick one with `i2c_id:` |
| [display](../components/display/README.md) | `display:` | the base of screens (`gm_screen_t`): panels the text displays (`displays:`, "Displays" below) are shown on |
| [led_strip](../components/led_strip/README.md) | `led_strip:` | the base of LED strips: LEDs that show any colour, a byte per colour ([leds "Full-colour LEDs"](../components/leds/README.md#full-colour-leds)) |
| [led_driver](../components/led_driver/README.md) | `led_driver:` | the base of LED drivers: a RAM of a bit per LED or segment |
| [sn74hc595](../components/sn74hc595/README.md) | `sn74hc595:` | shift-register outputs, written from the scan tick |
| [sn74hc165](../components/sn74hc165/README.md) | `sn74hc165:` | shift-register inputs, read from the scan tick |
| [cd74hc4067](../components/cd74hc4067/README.md) | `cd74hc4051:`, `cd74hc4067:` | 8- and 16-channel multiplexers, analog or digital |
| [mcp23017](../components/mcp23017/README.md) | `mcp23017:` | 16 GPIO on I2C (ESP only for now) |
| [pcf8574](../components/pcf8574/README.md) | `pcf8574:` | 8 GPIO on I2C (ESP only for now) |
| [decoder](../components/decoder/README.md) | `decoder:` | 74HC138-style one-of-N row selects, for matrix rows |
| [switch_matrix](../components/switch_matrix/README.md) | `switch_matrix:` | up to 32 switches, scanned from the scan interrupt |
| [xy_pad](../components/xy_pad/README.md) | `xy_pad:` | a resistive touch pad: two analog channels |
| [ws2812](../components/ws2812/README.md) | `led_strip:`, `platform: ws2812` | WS2812B / SK6812 strips (RGB or RGBW) |
| [maudio_pads](../components/maudio_pads/README.md) | `led_strip:`, `platform: maudio_pads` | the M-Audio Code pad LED board, on a serial line in its own protocol |
| [tm1628](../components/tm1628/README.md) | `led_driver:`, `platform: tm1628` | TM1628 and its relatives (TM1638, TM1620, ...): LEDs, with an optional fade |
| [max7219](../components/max7219/README.md) | `led_driver:`, `platform: max7219` | MAX7219 7-segment digits and LEDs (chained) |
| [tm1637](../components/tm1637/README.md) | `led_driver:`, `platform: tm1637` | TM1637 7-segment digits |
| [ht16k33](../components/ht16k33/README.md) | `led_driver:`, `platform: ht16k33` | HT16K33 14- and 7-segment backpacks and LED matrices, on I2C (ESP only for now) |
| [ssd1306](../components/ssd1306/README.md) | `display:`, `platform: ssd1306` | SSD1306 / SH1106 OLED panels, on I2C (ESP only for now) |
| [analog](../components/analog/README.md) | `analog:` | analog inputs: pots, faders, wheels, pressure strips, a control ("Controls" below) |
| [buttons](../components/buttons/README.md) | `buttons:` | buttons and switches, a control ("Controls" below) |
| [encoders](../components/encoders/README.md) | `encoders:` | rotary encoders, a control ("Controls" below) |
| [key_matrix](../components/key_matrix/README.md) | `key_matrix:` | the keyboard: switch or velocity keys, a control ("Controls" below) |
| [leds](../components/leds/README.md) | `leds:` | LEDs: what each shows, their looks and owners, the encoder rings, a control ("Controls" below); always built |
| [modes](../components/modes/README.md) | `modes:` | modes, layers and cycles, a control |
| [pads](../components/pads/README.md) | `pads:` | velocity pads, a control ("Controls" below) |

Each component's README has its YAML keys, its pins and how it runs. A device builds only the components it uses (`components.used()`: those whose hub keys and display platforms its YAML uses, and the controls it has, by each control's `in_device(dev)`, from `components.controls()`), those they depend on (`DEPENDENCIES`) and the required ones (`REQUIRED`, such as `hub` and `leds`): the builder writes their sources and include dirs into `gm_components.mk` (make) and `gm_components.cmake` (ESP-IDF), and their headers and what they add to the hub and screen structs into `gm_components.h`, next to `gm_device_config.h`.

The YAML keys a component had before are refused, with the new form in the message (`ssd1306:` is `display:` with `platform: ssd1306`; `ws2812:` and `maudio_pad_leds:` are `led_strip:`; `tm1628:`, `max7219:`, `tm1637:` and `ht16k33:` are `led_driver:`). `gemifi_build.py migrate --write <file>` rewrites a file to the new form ([tools/builder](../tools/builder/README.md)).

### Pins

Every pin in a device is a `gm_pin_t { hub, flags, num }` (`include/gm_pin.h`):
- `hub = 0`: a pin of the MCU. `num` is the platform's pin number: NUC1xx `port * 16 + bit` (`PA0` = 0, `PB12` = 28, `PC7` = 39); ESP32 the GPIO number.
- `hub = n ≥ 1`: pin `num` of hub `n - 1` (chained shift registers continue: pins 8–15 are on the second chip). Hubs are numbered in the order they appear in the YAML.
- `flags`: `GM_PIN_INVERT` (YAML `inverted: true`: active-low, pressed / lit = electrical 0), `GM_PIN_PULLUP`, `GM_PIN_PULLDOWN` (YAML `mode: INPUT_PULLUP` / `INPUT_PULLDOWN`; MCU pins, and pull-ups on MCP23017 pins), `GM_PIN_OPEN_DRAIN` (YAML `mode: OUTPUT_OPEN_DRAIN`; only a `tm1628` or `tm1637` `dio_pin`, platform mode `HAL_OUT_OD`).

A **hub** is a component that gives pins: every one above from `sn74hc595` to `ht16k33`. A pin on a hub names the hub by its list's key and its id: `{ sn74hc595: leds, number: 3 }`, `{ cd74hc4051: faders, number: 0 }`, `{ led_strip: strip, number: 0 }`, `{ led_driver: digits, number: 8 }`. The MCP23017's pin key is `mcp23xxx` (as in ESPHome): `{ mcp23xxx: io, number: GPB7 }`. How hub drivers run (scan tick or main loop), and their interface: [hub](../components/hub/README.md).

Inputs on HC165 are read once per scan into a cache, and outputs on HC595 are written once per scan, so a whole key matrix can live on shift registers. MCP23017 and PCF8574 are read and written from the main loop (every 1 ms), since I2C can't run in the scan interrupt.

### Matrices behind decoders

The M-Audio Code 61 (`devices/m-audio-code61.yaml`): rows come from 3-to-8 decoders that share address lines, and the keys, panel buttons and encoders all share the same columns. Declare the decoder once, then give each matrix a range of its outputs, `rows: { decoder: rows, first: 16, count: 4 }` (or a list of `{ decoder: rows, number: n }` pins). The key matrix and the switch matrices are scanned one after the other from the scan interrupt, so they can share column pins: set `allow_other_uses: true` on each use. A decoder output is selected when its row is driven and stays selected until the next row's select replaces it.

```yaml
decoder:
  - id: rows
    address: [PB3, PB4, PB5]
    groups: [{ PB6: 1, PB7: 0 }, { PB6: 0, PB7: 1 }, { PB6: 0, PB7: 0 }]   # outputs 0-7, 8-15, 16-23
switch_matrix:
  - id: panel
    rows: { decoder: rows, first: 16, count: 4 }
    columns:
      - { number: PB8, mode: INPUT_PULLUP, inverted: true, allow_other_uses: true }
      # ... PB9-PB15
buttons:
  - { id: play, pin: { switch_matrix: panel, number: 13 }, action: cc_momentary, cc: 113 }
```

### Adding a component

A folder, `components/<name>/`, named after its YAML key (or, on a base, its `platform:` name):

1. **`__init__.py`**: what the builder needs. A hub type lists `HubType` subclasses in `HUB_TYPES`, a screen platform `ScreenType` subclasses in `DISPLAY_PLATFORMS`; `DEPENDENCIES` names the components it builds on (`("led_strip",)`), `REQUIRED = True` builds it into every device. A control has `in_device(dev)` (whether a device has it, so whether it's built), `validate(v, dev, data)` for its YAML and functions for its generated C (`c_defines(dev)`, `c_tables(dev)`). Components reach what they share with the rest of the device (DAW roles, Mackie Control, MIDI messages) through the validator `v`, and the pin model, the record type and the YAML values through `components/api.py`. A chip that moves onto a base keeps its old keys in `old_keys`: they are refused with the new form, and `gemifi_build.py migrate` rewrites them. Its keys are data (`components/keys.py`; every control, hub type, display platform and the I2C buses has them, the modes not yet): a `Keys(Key(name, type, default=, doc=, see=, param=), ...)` that `validate` checks values with (`KEYS.get(v, d, "length", p)`), and that the JSON schema and the key table on its docs page are made from. `param=` names the device param a key starts (`encoder.cc`): the web page can change it, and a preset can set the per-preset ones. A control lists where its keys go in `KEY_SECTIONS`, a hub type or screen platform sets `spec`; `tools/builder/tests/test_keys.py` and `test_keys_hubs.py` check the validator takes what each type says.
2. **Its C** (`<name>.c`, and `<name>.h` if others need its types), compiled only into devices that use it. Its folder is an include dir when it has headers.
3. **`README.md`**: in this order, as the others do: a first paragraph that says what it is (it opens `A control (…):`, `A hub type ([hub](../hub/README.md)):`, `The base component of …` or names its base with a link, which is how the site's docs group it), a short YAML example, where its keys are described (with keys as data, `<!-- keys -->` on a line of its own: the docs site puts the key table there, and the README doesn't list them again); then the longer explanations, pins and behaviour; then `## Files`, how it runs and its tests.
4. **Tests**: a builder test of its validation and C, and a core test of its driver (in its `test/`, or `core/test/`).
5. **The schema and the key tables**: `python3 tools/builder/gemifi_build.py schema -o site/src/schema/device.json` and `python3 tools/builder/gemifi_build.py keys -o site/src/_data/component_keys.json`.

A **control** is called by name from the builder and the controller, not found through a list, so it also needs:
- **`tools/builder/gemifi_device.py`**: import it (`from components import ...`), call its `validate` from `validate()` and its `c_*` functions from `generate()`, where the generated header and tables are put together.
- **`core/include/gm_controls.h`**: its functions and the state it shares, each with its writer (a control has no `.h` of its own).
- **`core/src/controller.c`**: `gm_init` calls its init, `gm_poll` its poll, `gm_release_all` what it lets go of on a preset switch, each under its `#if` (`GM_N_PADS > 0` ...); a call the protocol or host feedback makes whatever the device has (`gm_analog_host` ...) gets a stub there that does nothing without it.
- **`core/src/settings.c`**: its params, if it has any.

What each kind implements: [Adding a hub type](../components/hub/README.md#adding-a-hub-type), [Adding a strip platform](../components/led_strip/README.md#adding-a-strip-platform), [Adding a driver platform](../components/led_driver/README.md#adding-a-driver-platform), [Adding a display platform](../components/display/README.md#adding-a-display-platform).

## Controls

The controls (buttons, LEDs, analog inputs, pads, encoders, the key matrix, and the modes that change what they do) are what the player plays. Each is a component: [buttons](../components/buttons/README.md), [leds](../components/leds/README.md), [analog](../components/analog/README.md), [pads](../components/pads/README.md), [encoders](../components/encoders/README.md), [key_matrix](../components/key_matrix/README.md) and [modes](../components/modes/README.md), built when the device has that control (its `in_device(dev)`), except leds, which every device builds (`REQUIRED`: the status LED and the lamp test). Each control's keys are in the key table on its docs page (made from its key spec, `components/keys.py`, as the editor schema is); its README has the longer explanations (button actions, LED looks, pitch wheels, pads, encoder modes and acceleration, the key matrix, modes, layers and cycles) and how it runs. The controller and the controls share state through `include/gm_controls.h`.

Every control has an `id` (what the rest of the file uses to refer to it) and/or a `name` (display text). See "Ids and names" below.

| Control | YAML (its keys: the component's key table) | Params it creates (per item, as arrays) |
|---|---|---|
| button | `buttons:`, `combos:`, `button_settings:` ([buttons](../components/buttons/README.md#keys)) | `button.action`, `button.number`, and once per device with that gesture `button.tap_time`, `button.hold_time`, `button.combo_time`, `button.repeat_delay`, `button.repeat_rate` |
| LED | `leds:`, `led_defaults:` ([leds](../components/leds/README.md#keys)) | – (LEDs show their button's state, or the octave/transpose level, or what the host sends) |
| analog (pot, slider, fader, expression, wheel) | `analog:` ([analog](../components/analog/README.md#keys)) | `analog.cc`, `analog.mode` |
| pad (velocity pad: pressure / FSR) | `pads:`, `pad_settings:` ([pads](../components/pads/README.md#keys)) | `pad.note`, and once per device `pad.channel`, `pad.curve`, `pad.aftertouch`, `pad.threshold`, `pad.release`, `pad.full`, `pad.window`, `pad.retrigger` |
| encoder | `encoders:`, `encoder_settings:` ([encoders](../components/encoders/README.md#keys)) | `encoder.cc`, `encoder.mode` |
| key matrix (at most one) | `key_matrix:` ([key_matrix](../components/key_matrix/README.md#keys)) | the velocity params if `type: velocity` |

Global params (always present): `midi.channel`, `midi.transpose`, `midi.octave`, `bend.glide`, `ramp.time`, `debug.events`, `debug.log_level`, and the read-only `config.hash` ("Config hash" below). The list with ids is in `src/settings.c`; ids never change meaning.

### MIDI messages

Besides notes, 7-bit CCs, pitch bend, pressure and program changes, a control can send 14-bit CCs, NRPN and RPN, bank select with a program change, panic, MMC transport commands and SysEx strings of your own. Each kind is compiled in only when the YAML uses it (or lists it in `midi_messages.enable`), so a device that uses none of them gets the same image as before.

```yaml
midi_messages:                 # all optional
  enable: [cc14, nrpn, program, panic, mmc]   # compile these in for the web page, even if nothing uses them yet
  null_rpn: true               # after each NRPN / RPN message, the null RPN (101 / 100 = 127); default false
  mmc_device_id: 0x7F          # the MMC target, 0..0x7F; default 0x7F (all devices)
  sysex:                       # strings in hex, F0 to F7, with placeholders
    - { id: gs_volume, data: "F0 41 10 42 12 [40 00 04 v] cs F7" }   # Roland GS master volume
    - { id: cutoff, data: "F0 43 ch 7F msb lsb F7" }

buttons:
  - { id: stop, pin: PB0, action: mmc, mmc: stop }
  - { id: play, pin: PB1, action: mmc, mmc: play }
  - { id: panic, pin: PB2, action: panic }
  - { id: piano, pin: PB3, action: program, program: 0, bank: { msb: 121, lsb: 0 }, channel: 2 }
  - { id: volume_max, pin: PB4, action: sysex, sysex: gs_volume, max: 100 }
  - { id: sustain, pin: PB5, action: cc_momentary, cc: 64, min: 0, max: 100 }
analog:
  - { id: fader_1, pin: PA0, mode: cc14, cc: 7 }          # MSB on CC 7, LSB on CC 39
  - { id: fader_2, pin: PA1, mode: nrpn14, nrpn: 1234 }
  - { id: knob, pin: PA2, mode: sysex, sysex: cutoff, channel: 3 }
  - { id: expr, pin: PA3, cc: 11, min: 127, max: 0 }      # reversed
encoders:
  - { id: encoder_1, pin_a: PB6, pin_b: PB7, cc: 20, mode: relative_sign }
  - { id: encoder_2, pin_a: PB8, pin_b: PB9, mode: rpn14, rpn: 0 }   # pitch bend range
```

- **14-bit CC** (`mode: cc14`, analog inputs and encoders): the MSB on `cc` (0..31), then the LSB on `cc` + 32. A fader then has the ADC's resolution: it sends when it moves by 3 raw counts (half the pitch bend's noise step), so about 1300 steps over its travel instead of 128.
- **NRPN and RPN** (`mode: nrpn`, `nrpn14`, `rpn`, `rpn14`; the number in `nrpn:` / `rpn:`, 0..16383): CC 99 / 98 (NRPN) or 101 / 100 (RPN) with the number's MSB and LSB, then data entry: CC 6 with the 7-bit value, or CC 6 and 38 with the 14-bit one. `null_rpn: true` adds the null RPN after each, so a later data entry from anything else goes nowhere. Every message sends the number again, so a receiver that missed one still gets the next.
- **Numbers.** The number is the `analog.cc` / `encoder.cc` param, so a preset or the web page can change it. On a device with NRPN or RPN, those params go up to 16383; a CC mode then uses the number's low 7 bits. That costs no RAM, but on the stm32f1's packed store those values take 2 bytes instead of 1 ("Storage" in "Presets"; the builder counts it).
- **Encoders.** `mode:` sets an encoder's default (`absolute`, the relative modes, or the absolute 14-bit / NRPN / RPN ones). `relative_sign` is sign-magnitude: 1..63 up, 65..127 down (some DAWs want this one, others two's complement or offset 64). An absolute 14-bit encoder moves 1/256 of its range per detent, or more with acceleration.
- **Bank select.** `program` sends a Program Change of `program:` (0..127). `bank:` on a `program`, `program_next` or `program_prev` button sends Bank Select first: CC 0 = bank >> 7, then CC 32 = bank & 127. Give it as 0..16383 or as `{ msb, lsb }`. A preset takes `bank:` next to its `program:` (the `preset.bank` param, per preset).
- **Panic** (`action: panic`): first gemifi lets go of what it holds, as a preset switch does: Note Off for held keys, note buttons and pads, CC momentary buttons back to their off value, ramps and pitch bend back. Then All Sound Off (120), All Notes Off (123) and Reset All Controllers (121) on all 16 channels, 144 bytes. The keys and buttons still held then send nothing when let go.
- **MMC** (`action: mmc`, `mmc:` one of `stop`, `play`, `deferred_play`, `fast_forward`, `rewind`, `record`, `record_exit`, `record_pause`, `pause`, `eject`, `chase`, `reset`, or a number 1..127): `F0 7F <device> 06 <command> F7` on press.
- **SysEx strings** are fixed at build time and stored in flash. `data:` is hex bytes from F0 to F7. Placeholders: `v` the value (7-bit), `msb` / `lsb` its top and bottom 7 bits (a 14-bit value), `ch` the control's channel (0..15). `[ ... ] cs` is a Roland checksum: `cs` becomes (128 − (sum of the bytes in the brackets) % 128) % 128. A string sends at most 64 bytes. An analog input with `mode: sysex` sends it with its value; a button with `action: sysex` sends it with `max:` on press and `min:` on release (127 / 0, or 16383 / 0 for a 14-bit string; the release only if the string has a value). Its number is the string's index (`sysex:` takes the id).
- **Channel, `min:` and `max:`** on a button, analog input or encoder (build time only). `channel:` (1..16) sends on that channel instead of `midi.channel`; pitch bend buttons stay on `midi.channel`. `min:` / `max:` map the value range onto min..max, in the mode's units (0..127, or 0..16383 in a 14-bit mode); min greater than max inverts it. A CC button sends `max` for on and `min` for off; a relative encoder with min > max turns the other way. Layer twins have their physical control's. They are tables in flash, not params: they are the same in every preset and the web page can't change them. Per-preset params would need a byte per control and preset, which the Code 61's store (4 slots, 1460 of 1744 B) doesn't have room for.
- **What comes back.** Host feedback updates absolute encoders and pick-up for these kinds too: a 14-bit CC's MSB then LSB, and an NRPN or RPN (the device keeps the last number chosen on each channel, then takes CC 6 / 38). A control with a `channel:` listens on that channel.
- **DIN MIDI.** SysEx and MMC go to DIN Out like other messages. With `cc14`, CCs 32-63 aren't coalesced, so an LSB never goes out before its MSB.
- **The schema.** The new values have fixed numbers (`button.action` 17 program, 18 panic, 19 MMC, 20 SysEx; `analog.mode` 3..8; `encoder.mode` 3..8). An option a device was built without has an empty label: the firmware refuses it and the web page doesn't offer it (protocol/sysex.md §4.4).
- **Cost.** Keystation Mini 32 with every kind on: +1.1 KB of flash and 44 bytes of RAM (26.7 of its 32 KB). Code 61: +1.7 KB, 32 bytes of RAM. With none on, +28 bytes (Keystation) and +64 bytes (Code 61). RAM with NRPN feedback: 32 bytes (the last number per channel).
- **Generated:** `GM_MSG_CC14`, `GM_MSG_NRPN`, `GM_MSG_PROGRAM`, `GM_MSG_PANIC`, `GM_MSG_MMC`, `GM_N_SYSEX` (and `GM_NULL_RPN`, `GM_MMC_DEVICE`, `GM_SYSEX_MAX`) in `gm_device_config.h`; with them on, `GM_BTN_CTL`, `GM_ANALOG_CTL`, `GM_ENC_CTL` (`gm_button_ctl[]`, `gm_analog_ctl[]`, `gm_encoder_ctl[]`: `gm_ctl_t` min, max, channel), `GM_BTN_BANK` (`gm_button_bank[]`), `GM_ENC_MODES` (`gm_encoder_mode_defaults[]`) and `gm_sysex[]` (`gm_sysex_t`, `gm_device.h`).
- **Tests:** `core/test` `test_msg` (`msg-test.yaml`), the builder's `TestMidiMessages`.

## Software scan

Without `hardware_scan:`, the key matrix is scanned from the scan tick (`scan_interval`): each row in turn is driven, the columns are read after `settle`, and the row is released. Rows on MCU pins are driven by one store each (`hal_pin_out_reg`), and columns on MCU pins are read as runs of adjacent bits of their port (`hal_pin_in_reg`; the Keystation's PB0–PB5, PB8, PB12 are 3 runs from one load), not pin by pin.

When every row is such a pin, every column is on one input register, nothing is on a shift-register hub and a key's contacts are on columns (the Keystation), the scan has a faster loop of its own: [key_matrix "How it runs"](../components/key_matrix/README.md#how-it-runs).

## Hardware scan

On a platform whose timers and DMA can do it (stm32f1 for now), `hardware_scan:` takes the matrix scan off the CPU: the key matrix and every switch matrix are scanned row by row by the hardware, and the core only looks at finished frames.

```yaml
hardware_scan:
  matrix:                      # the key matrix and switch matrices
    row_time: 3.5us            # time per row; a frame is every row once (23 rows on the Code 61, padded to 24)
    settle: 3us                # row selected -> columns read; at least 0.25us (0.5us with precharge)
    precharge: false           # drive the columns to their idle level between rows (below)
    timer: auto                # stm32f1: the timer that paces it, auto or tim1..tim4 ("DMA channels")
  analog: true                 # analog inputs and pads: mux steps + ADC by timer + DMA (below);
                               # or `analog: { timer: tim3 }` (auto, tim2..tim4)
```

- **What it needs.** Every row (key matrix and switch matrices) is selected by one store (decoder rows on one port with a set/reset register, `hal_pin_bsrr`), and every column is a bit of one input register. The Code 61 fits: PB3–PB7 select, PB8–PB15 read. If a device doesn't fit, the firmware logs a WARN at boot and scans in software as before. A frame is one reading of each row; an odd row count gets its last row twice, so frames are whole 32-bit words.
- **Timing.** A key's two contacts are timed in frames (`row_time` × rows: 84 us on the Code 61 at 3.5 us), instead of the `scan_interval` tick, and velocity is worked out in microseconds, so finer timing gives finer velocities at the fast end. The scan tick keeps running for whatever else it does (shift-register hubs, encoders on MCU pins); matrices, encoders on switch matrices and the key timers' upkeep move to the frames. With nothing else left (the Code 61), the stm32f1 firmware doesn't start the tick at all (`gm_scan_tick_needed()`). The panel's `rows_per_scan` doesn't apply: every row is in every frame.
- **Precharge.** A column that a closed key pulled low has only the MCU's weak pull-up (30–50 kΩ) to come back up, which is why `settle` has to be microseconds. With `precharge: true` the columns are driven to their idle level (the level their pull-up or pull-down gives) for a moment before each row, so the read only waits for a closed contact to pull its column down. On the Code 61 the keys are fine at `row_time: 1.5us, settle: 1us` (36 us frames), but a lightly pressed panel button (a carbon contact of a few kΩ) can need longer, and then reads as let go mid-press: its YAML uses `3.5us` / `3us` (84 us frames). While a key on the row being left or entered is held, its column is briefly driven against the row (about 0.25 us per row), so it needs a matrix with diodes or tolerant drivers. It's on in the Code 61 YAML (tried on hardware: no ghost or missing notes); leave it off on other boards until tried. With precharge, every column must be in one of the port's two config registers, set up as a pulled input, and nothing else may reconfigure those pins while the scan runs.
- **Buttons** on a switch matrix are read in every frame (in software, on every scan of their row), and debounced from those scans as in software ([buttons "How it runs"](../components/buttons/README.md#how-it-runs)).
- **Checks.** The builder wants `row_time` ≥ `settle` + 0.25 us (+ 0.5 us with precharge), and `settle` ≥ 0.5 us with precharge. At boot the stm32f1 logs the frame length, the longest frame-processing interrupt and how often it fell behind (a flash write stalls it; frames lost then still count as time).
- **Analog** (`analog: true`): the analog inputs and pads are read in sweeps by the hardware. Every mux used must share the same select lines (one set/reset register), and nothing may read a mux digitally (a button on a mux). Each step of a sweep stores the select lines, waits the muxes' `delay:`, and converts every mux's common pin plus every analog input or pad on an ADC pin. The Code 61's 8 steps × PA7, PA6, PA1, PA0 take 200 us. `vpin_analog()` then returns the latest reading (an ADC pin: the mean of its readings in the sweep), the analog inputs are all read every millisecond instead of one every 2 ms, and the pads see their strongest reading of the sweeps since their last pass (about 5), so a strike's peak isn't lost between passes. The main loop does no conversions or mux settles at all.
- **Code.** The core side is in `controller.c` (`gm_scan_plan()`, `gm_scan_frames()`, `gm_controller.h`), the key matrix's in `components/key_matrix/key_matrix.c` (`matrix_plan()`, `matrix_frame()`, `matrix_retry()`) and `vpin.c` (`vpin_scan_plan()`, `vpin_scan_feed()`, `gm_adc_plan()`, `gm_adc_sweep()`, `vpin_analog_peak()`); the STM32F1 drivers are `platform/stm32f1/src/scan_dma.c` and `adc_dma.c`. Host tests: `core/test` `test_matrix_hw` (`matrix-hw-test.yaml`) and `test_pads_hw` (`pads-hw-test.yaml`).

### DMA channels (stm32f1)

The STM32F103 has 7 DMA channels, and each peripheral's DMA request is wired to one of them (RM0008 table 78). The YAML decides which features get DMA, and the builder hands out the channels:

| Feature | DMA requests | Without DMA |
|---|---|---|
| `hardware_scan.matrix` | 2 of one timer (select, read); with `precharge` 4 (+ columns in, precharge) | the scan interrupt reads the matrix |
| `hardware_scan.analog` | ADC1 (channel 1) + 1 of one timer (mux step) | the main loop reads one input per pass |
| `maudio_pads` strip (`dma: true`, the default) | USART2_TX (7) | `dma: false`: the USART2 interrupt sends the bytes |
| `din_midi` (`dma: false`, the default) | `dma: true`: USART1_TX (4), USART1_RX (5) | the USART1 interrupt moves the bytes |
| `ws2812` strip on PA7 / PB15 | SPI1_TX (3) / SPI2_TX (5) | none: a strip always sends by DMA (move it to the other pin) |

The UARTs' and SPIs' channels are fixed (two of them on one channel, as a PB15 strip and `din_midi dma: true`, is an error). The scans' depend on the timer that paces them, since each timer's update and compare events are wired to their own channels:

| Timer | UP | CH1 | CH2 | CH3 | CH4 | Matrix | Analog (ADC1 trigger) |
|---|---|---|---|---|---|---|---|
| TIM1 | 5 | 2 | 3 | 6 | 4 | yes (5 channels to choose from) | no: its triggers drive CC outputs, and PA9/PA10 are USART1's |
| TIM2 | 2 | 5 | 7 | 1 | 7 | yes (CH2 and CH4 share 7; precharge needs channel 1, ADC1's) | CC2 (mux on CH1 or CH4) |
| TIM3 | 3 | 6 | – | 2 | 3 | no precharge only (3 channels) | TRGO = OC2REF (mux on CH1, CH3 or CH4) |
| TIM4 | 7 | 1 | 4 | 5 | – | yes (precharge needs channel 1, ADC1's) | CC4 (mux on CH2 or CH3) |

`timer: auto` (the default) tries the matrix on TIM1, TIM3, TIM4, TIM2 and the analog scan on TIM4, TIM3, TIM2, taking the first that leaves the UARTs' channels free. For the Code 61 that is the original map: matrix on TIM1 (select = update, ch5; columns in = CC1, ch2; read = CC2, ch3; precharge = CC3, ch6), analog on TIM4 (mux = CC2, ch4; ADC1 by CC4). Only the select may be the update event; when it is a compare instead, every step of the row comes one timer tick (14 ns) later, which changes nothing else. `timer: tim3` (etc.) fixes the timer, and auto still picks its compares. TIM2 is also the core's scan tick (an interrupt, no DMA): when a scan takes it, the tick moves to TIM3 or TIM4.

When nothing fits, the build stops. The message names both users and says what would fit: `din_midi.dma: needs DMA1 channel 4 (USART1_TX), which hardware_scan.analog uses (TIM4_CH2): set din_midi dma: false (...), or turn hardware_scan.analog off; or turn hardware_scan.matrix precharge off: the scan then needs 2 channels instead of 4, and that fits`. An explicit timer that can't run its scan is an error on its `timer:` line, with the timer's requests. The build output and the top of `gm_device_config.h` show the map and the timers, for example `DMA1 channels: 1 ADC1 (hardware_scan.analog), 2 TIM1_CH1 (hardware_scan.matrix columns in), ... 7 USART2_TX (maudio_pads 'pad_leds'); free: none` and `timers: TIM1 hardware_scan.matrix, TIM2 scan tick, TIM4 hardware_scan.analog (ADC1 started by TIM4_CC4)`.

What fits on the Code 61 (ADC1, the mux step and the matrix leave 7 − 2 − 4 = 1 channel with precharge, 3 without):

| Matrix | Fits with it |
|---|---|
| precharge (36 us frames) | `maudio_pads` DMA (the default), DIN by interrupt |
| no precharge (`row_time: 3.5us, settle: 3us`: 84 us frames) | `maudio_pads` DMA and `din_midi dma: true`: matrix on TIM1 (select CC1 = ch2, read CC2 = ch3), analog on TIM3 (mux CC1 = ch6, ADC1 by TRGO), DIN on 4 and 5 |

The UARTs gain little from DMA: at 31250 baud a DIN MIDI byte takes 320 us, and the interrupt for it takes about a microsecond. Spend the channels on the scans, and use DMA for the UARTs on boards that don't scan by hardware.

## Displays

A display is a row of segment characters on output pins of hubs (a TM1628, MAX7219, TM1637, 74HC595 ...), or a row of text on a screen (`display:`, an OLED). Up to 4 displays, of up to 16 characters each (a text display up to 21), one row each.

- **Types.** `segment14` (the default; the Code 61's): 15 pins a character, segments a b c d e f g1 g2 h i j k l m, then the dot. `segment7`: 8 pins a character, segments a b c d e f g, then the point. Both are drawn in `core/include/gm_display.h`. `text`: 5 × 7 characters on a screen (below). Letters show as capitals (on 7 segments as best they can: b, d, n, r and t in lower case, K M W X approximated), a `.` lights the previous character's dot, and characters with no glyph are blank. The fonts are `gm_seg14_glyph()` and `gm_seg7_glyph()` in `core/src/display.c`.
- **Text displays** (`type: text`) are on a screen, a `display:` item ([components/display](../components/display/README.md); an OLED is `platform: ssd1306`, [components/ssd1306](../components/ssd1306/README.md)), placed by text line (8 pixels a line from the top) and pixel column ([the display keys](yaml/displays.md#keys)). At scale 1 a character is 6 × 8 pixels with its gap, so 21 fit a row on 128 pixels; at scale 2 it is 12 × 16 pixels, 2 lines tall, 10 a row. Every printable ASCII character has a glyph, lower case too (the classic public-domain 5 × 7 font, `gm_text_glyph()` in `core/src/display.c`), and a `.` takes a place. Several text displays can share a screen, one per line or side by side; the builder refuses ones that overlap.
- **Pins.** Each item of `chars:` is a character, left to right:
  - its first pin (segment a): the others are the pins after it, in a row (a character's segments on one digit's lines, the usual wiring);
  - with `segment_step: n` on the display, segment k is n × k pins after the first: characters on the SEG lines and segments on the grids (a TM1628 in 7-grid mode: `segment_step: 16`, character i's first pin `number: i`). The point is only wired where it fits on the hub;
  - or a list of its segments' pins, in segment order (the dot may be left out): any other wiring.

  `segment_order:` on the display names the segments in pin order from each first pin, for a module whose segments are in a row but in another order: `[dp, a, b, c, d, e, f, g]` (segment7: `a`–`g`, `dp`; segment14: `a`–`f`, `g1`, `g2`, `h`–`m`, `dot`; `dot` and `dp` are the same). Segments it leaves out aren't wired, and their pins stay free.
  Every segment pin counts as used, so an LED on one is an error. Segments are hub pins (the display writes them from the main loop through the hub).
- **What it shows** of its own, `shows:`, while no host is writing to it:

  | `shows:` | |
  |---|---|
  | `last_message` | display 0's default. "GEMIFI" at boot, then the last MIDI message the device sent: a CC as its number and value (`74 127`), a Note On as its name and velocity (`C#4.127`, middle C = C4), a program change as `PC 12`; Note Offs, pitch bend and pressure leave it as it is; when the two parts would touch, a dot separates them. And the device's messages (below). A message or a program change isn't replaced by a CC or a note for 1.5 s (so letting go of Shift after Shift + Up leaves `PC 23`) |
  | `preset_number` | the active preset's number, right-aligned (`  3` on 3 digits; the last digits if it's too narrow). A floor board's display |
  | `preset_name` | its name (from the YAML or the editor), scrolled once if it's longer |
  | `channel` | `CH <n>` (`CH.5` on 3 characters), or the number where that won't fit |
  | `none` | the others' default: blank, for a host's text only |

- **Messages.** A preset switch (from a button, a mode or the editor) shows the preset's name, a channel change on the device (a mode's `channel`, `channel_up`, `channel_down`) shows `CH <n>`, also when a hold mode that changes the channel starts, an octave, transpose or pad octave press (a button or a mode's key) shows the new value (`OCT +1`, `TR -12`, `PAD 0`, also at a limit), and the safety prompts (`HOLD ...`, `RESET?`) show too, on the `last_message` displays. A message stays until the next MIDI message, but at least 1.5 s for a CC or a note (another message or a program change replaces it at once). One longer than the display scrolls once: the start for 1 s, a character every 300 ms, the end for 1 s, then the start again (a `preset_name` display scrolls its name the same way). `gm_display_message()` shows one; in a message every character takes a place, a `.` too.
- **From a host.** GET_COMPONENT lists each display (kind 6, by index in YAML order: its `id`, its role from `daw:`, the number of characters, one row, type characters), and FB_TEXT (`protocol/sysex.md` §4.6.2) writes to display n from a column, every character taking a place (a `.` too), clipped at the end of the row. The host's text covers the device's own, which carries on underneath. The device's own news (a message: a preset's name, `CH <n>`, a mode's name; or a program change it sent, `PC 23`) shows over it for 1.5 s (a long message until it has scrolled once), then the host's text is back; its plain MIDI (CCs, notes) doesn't. FB_RELEASE 0 or 3 hands every display back.
- **Text.** `gm_display_text(d, "...")` sets display d's text; `gm_display_preset()` and `gm_display_channel()` give it the values (presets.c and the controller call them only when a display shows them).
- **Timing.** MIDI messages are only recorded where they're sent (the main loop, keys too). The main loop formats them, steps scrolling text, and writes only the segments that changed, with the LED update (every 10 ms), and the hub's next send or flush puts them out.
- **Generated code:** `GM_N_DISPLAYS`, `GM_DISPLAY_CHARS` (the widest display: the RAM is sized by it, 4 bytes a character a display), `GM_DISPLAY_SHOWS` and `GM_DISPLAY_TYPES` (bits of the `shows:` values and types used: only their code is built) and `GM_DISPLAY_MAP` in `gm_device_config.h`; `gm_displays[]` (`gm_display_t`: type, character count, first pins or the segment map, `shows`, and a text display's scale, its screen (`gm_screens[]`, [components/display](../components/display/README.md)) and the framebuffer byte it starts at, line × width + x) in `gm_device_config.c`.

```yaml
displays:
  - id: display
    type: segment14
    daw: param                 # what a DAW script shows on it
    chars:                     # left to right: each character's segment a
      - { led_driver: leds_a, number: 48 }
      - { led_driver: leds_a, number: 64 }
      - { led_driver: leds_a, number: 0 }
  - id: bank                   # 2 digits with their segments on a TM1628's grids
    type: segment7
    shows: preset_number
    segment_step: 16
    chars: [{ led_driver: leds_b, number: 0 }, { led_driver: leds_b, number: 1 }]
```

Each key, its range and default: [the display keys](yaml/displays.md#keys).

Not yet: HD44780 character LCDs, SPI OLEDs, more than one row a display (use one display a line), graphics, and the TM1640. See #120.

## Layout

`layout:` says where each control is on the device, so the web page can draw it and light up the control being moved (its Tester view and "Find a control"). The device sends it on request (GET_LAYOUT, `protocol/sysex.md` §4.7). Without `layout:` the firmware has no layout and pays nothing for it, and the page shows the controls as a list.

```yaml
layout:
  size: [80, 30]               # optional: the drawing's size; default: just big enough
  keys: [0, 12, 76, 14]        # optional: where key_matrix goes; default: under the controls
  controls:
    volume: { x: 1, y: 1, w: 4, h: 4 }        # by id
    fader_1: { x: 7, y: 1, w: 2, h: 10, shape: fader }
    play: [11, 1]              # [x, y], with the shape's size
    stop: [11, 4, 3, 2]        # [x, y, w, h]
    xy_x: { x: 20, y: 1, w: 10, h: 10, shape: xy }   # one axis of an xy_pad draws the pad for both
```

- **Units.** Whole units of about 5 mm, from the top left, at most 255 each way. The page scales the drawing to fit.
- **Controls.** Buttons, analog inputs, encoders, pads and displays, each once, by its `id`. A control left out isn't drawn; the page lists it under the drawing. An id that names no control is a warning and isn't drawn, so a device built on a package can `!remove` a control the package's layout places. Layer twins are the same physical control as their base, so they aren't listed: the page shows them where the base is.
- **Shapes.** Buttons: `button` (the default, 3 × 2), `round_button` (2 × 2), `pad`. Analog inputs: `knob` (the default, 3 × 3), `fader` (2 × 8), `wheel` (2 × 7), `xy` (8 × 8). Encoders: `encoder` (the default, 3 × 3), `knob`. Pads: `pad` (the default, 4 × 4), `button`, `round_button`. Displays: `display` (10 × 3). Without `w` and `h` a control takes its shape's size.
- **Keys.** With a `key_matrix`, the builder adds the keys by itself: under the controls, from x 0, a white key 4 units wide and 14 high. `keys:` places them elsewhere. Which keys are black comes from `first_note`, so a keyboard needs nothing more.
- **Cost.** 5 bytes, plus 8 a control (10 for the keys, 9 for an XY pad), and the GET_LAYOUT code (about 150 bytes on a NUC120). The Keystation's layout is 79 bytes, the Code 61's 616.
- **Generated code:** `GM_LAYOUT_BYTES` in `gm_device_config.h` and `gm_layout[]`, the GET_LAYOUT blob, in `gm_device_config.c`.

### Raw readings

For calibrating and porting, a host can ask for the readings as the scan sees them (MONITOR and RAW, `protocol/sysex.md` §4.8): each button's pin, every contact of the key matrix, the ADC readings before `adc_min`/`adc_max`, encoder detent counts and pad pressure. The device sends one short message every 20 ms from the main loop, so the scan and the keys' MIDI aren't slowed, and stops by itself 5 s after the host's last request. The web page's Tester view uses it, and writes `adc_min`/`adc_max` from a pass of moving every control end to end. GET_COMPONENT reports each analog input's `adc_min` and `adc_max` for it. It costs about 700 bytes of flash on a NUC120 (`GM_MONITOR 0` in a build leaves it out).

## DIN MIDI

The 5-pin MIDI jacks are a UART at 31250 baud (stm32f1: USART1, TX PA9, RX PA10). `din_midi.mode` sets how they reach USB:

| `mode` | The computer sees | DIN In goes to | DIN Out gets |
|---|---|---|---|
| `usb_merge` (default) | only the device's own port | the device's port, mixed with the keys | the host's channel and realtime messages on the device's port, when `midi.usb_to_din` is on. Never its SysEx: that is the gemifi protocol's |
| `separate_port` | a second USB-MIDI port, `port_name` (default "<friendly_name> MIDI") | that port | everything the computer sends to that port |
| `din_only` | only the device's own port | nowhere (only the thru) | nothing from USB |

`separate_port` has a known problem: on Windows, a device with two USB-MIDI ports can hang Chrome's Web MIDI (the web updater) for good when it leaves or comes back on USB (issue #54). The builder warns when you choose it.

In every mode, two settings (MIDI group, stored with the globals) decide the rest:

- `midi.din_out`: the device's own messages (keys, pads, CCs, program changes) also go to DIN Out. On by default.
- `midi.thru`: DIN In is copied to DIN Out. Off by default.

```yaml
din_midi:
  tx_pin: PA9
  rx_pin: PA10                 # optional: without it, MIDI Out only
  mode: usb_merge              # usb_merge | separate_port | din_only
  port_name: Code 61 MIDI      # separate_port only
  thru: false                  # start values of the settings
  controller_out: true
  usb_to_din: false            # usb_merge only
  dma: false                   # stm32f1: DMA channels 4 and 5 ("DMA channels")
```

- **Whole messages.** DIN Out mixes three sources: the device, the thru, and USB. Each message goes out whole. While one source's SysEx is going out, the others' messages wait (up to 16: 8 for the thru's and USB's, 4 kept for the device's Note Offs) and follow its F7; a device Note Off that finds no room ends the SysEx there with F7 and drops its rest, since a stuck note is worse. Realtime bytes such as clock go straight through, which MIDI allows. A SysEx from a second source meanwhile is dropped, and one that stalls for 500 ms is ended with F7.
- **Priority.** The thru's and USB's messages join DIN Out only while under 64 bytes (20 ms) wait to go out, so however fast the host sends (`usb_to_din` passes on its LED feedback too), the device's own notes never wait behind more than that; the rest is dropped and counted. Their Note Offs, realtime bytes, SysEx and the device's own messages may fill the platform's queue to 448 bytes, and the device's Note Offs have the last 64 to themselves, so they aren't lost even when everything else is.
- **Running status.** DIN Out leaves out a channel message's status byte when it's the same as the last one sent, as MIDI 1.0 recommends, so a stream of notes or of one controller takes 2 bytes a message instead of 3. A Note Off with velocity 0 or 64 (the device's own all have 0) right after a Note On on the same channel goes as Note On velocity 0, which every receiver takes as Note Off; one with another release velocity keeps its own status. Realtime bytes don't change the running status; SysEx and system common end it, so the next message has its status byte again. So does a message 250 ms or more after the last status byte, even in a steady stream, so a receiver plugged in mid-stream catches up within a quarter of a second. It's always on (build with `-DGM_DIN_RUNNING_STATUS=0` for a receiver that can't take it).
- **Coalescing.** Pitch bend, channel pressure and continuous CCs only matter by their latest value. Each waits in a slot for its channel and controller (8 slots) while 16 bytes or more (5 ms) wait to go out or a SysEx is going out, and a newer value replaces the one waiting. The slots go out in turn as the queue drains, and all before the next note or other message, so they keep their place ahead of it. So a pitch wheel, mod wheel and aftertouch moved together (6 bytes a millisecond, twice what DIN carries) keep the queue under 20 bytes and end on their latest values, instead of piling up for seconds. CCs whose order counts are never coalesced: bank select (0, 32), data entry (6, 38), increment / decrement and (N)RPN (96–101), the switches such as sustain (64–69), the velocity prefix (88) and channel mode messages (120–127).
- **DIN In.** It's parsed with running status, realtime inside SysEx, and system common. A status byte inside a SysEx ends it. On `separate_port`, SysEx streams to USB as it arrives. On `usb_merge`, a SysEx goes to USB whole after its F7, so it can't split the device's own messages. One over 256 bytes is dropped there.
- **Limits.** DIN Out is about 3 bytes a millisecond. A message that doesn't fit (above) is dropped whole and counted (`din out drops` in the stm32f1 timing log, with UART `in overruns`). The limits assume stm32f1's 512-byte queue; `GM_DIN_OUT_MAX` (448) moves the 448 for another size. `separate_port` uses the second USB cable, so it can't be combined with the DAW port (`mcu:`); use `usb_merge` there.
- **Platform:** `hal_din_open` / `hal_din_write` / `hal_din_read` / `hal_din_tx_queued` and `hal_midi_send_usb` (gm_hal.h). Only stm32f1 has them so far. Without `hal_din_tx_queued` the queue looks empty to the core: no coalescing, priority or Note Off reserve, only the platform's own "full".

## Device YAML

Format 2 (`gemifi: 2`). It follows ESPHome's conventions where ESPHome has one: the pin schema, ids, `name`/`friendly_name`, hub components and time periods.

For what a choice costs at run time (scan rates, settles, hubs, analog rates), see [performance.md](performance.md).

```yaml
gemifi: 2                      # format version
name: keystation-mini32        # node name: lowercase letters, digits and -, at most 31; names the built images
friendly_name: Keystation Mini 32   # display name: USB product string, HELLO, web UI (default: name)
project:
  version: 0.1.3               # firmware version (major.minor.patch): HELLO, .gfw header; default 0.1.0
board_id: 0x0001               # protocol/sysex.md section 7; 0x3F00-0x3FFF = DIY/private
platform: nuvoton-nuc1xx       # nuvoton-nuc1xx | stm32f1 | esp32 | esp32s2 | esp32s3
chip: NUC120LC1BN              # NUC: part; ESP: PlatformIO board id (e.g. esp32-s3-devkitc-1)
board: keystation-mini32-mk3   # stm32f1 / nuvoton-nuc1xx: the board profile ("Board profiles"); default per platform
usb: { vid: 0x1209, pid: 0x0002 }   # native-USB platforms
midi: { transport: usb }       # ESP only: usb (S2/S3) | serial | ble (ESP32)
i2c: { sda: GPIO8, scl: GPIO9 }     # only if an I2C hub or display is used (several: components/i2c)
scan_interval: 125us           # matrix/encoder scan period
hardware_scan: { matrix: { row_time: 3.5us, settle: 3us } }   # stm32f1: timer + DMA scanning ("Hardware scan")
din_midi: { tx_pin: PA9, rx_pin: PA10 }  # stm32f1: 5-pin MIDI ("DIN MIDI"), on the main USB port
midi_messages: { enable: [cc14] }   # 14-bit CC, NRPN, bank select, panic, MMC, SysEx ("MIDI messages")
boot_combo: [pitch_down, pitch_up]  # button ids
factory_reset_combo: [oct_down, oct_up]   # held at power-up for 5 s: wipe the settings ("Factory-reset combo")
unlock: { combo: [pitch_down, pitch_up], hold: 2s }   # opt-in: uploads etc. need the user's OK ("Physical unlock")
status_led: edit_led           # blink codes ("Status LED")
write_to_flash: false          # what the device stores on its own (below); host saves always work
layout: { controls: { volume: [1, 1] } }   # where the controls are, for drawing the device ("Layout")

cd74hc4051:
  - id: faders
    pin_s0: GPIO4
    pin_s1: GPIO5
    pin_s2: GPIO6
    common_pin: GPIO1
    delay: 5us
sn74hc595:
  - id: leds
    data_pin: GPIO10
    clock_pin: GPIO11
    latch_pin: GPIO12
    sr_count: 2

buttons:
  - id: sustain
    pin: { number: PA1, inverted: true }
    action: cc_momentary
    cc: 64
    led: sustain_led
  - id: oct_down
    name: Oct -                # display name; without it: "Oct down"
    pin: { number: PA3, inverted: true }
    action: octave_down
  - name: Shift                # only a name: the id is derived from it (shift)
    pin: { number: GPIO9, mode: INPUT_PULLUP, inverted: true }
    action: note
    note: 60

leds:
  - id: sustain_led
    pin: { number: PA12, inverted: true }
  - id: oct_down_led
    pin: { number: PA10, inverted: true }
    pin2: { number: PA14, inverted: true }
  - id: led_1
    pin: { sn74hc595: leds, number: 0 }

analog:
  - id: volume
    pin: PA0
    cc: 7
    adc_min: 32
    adc_max: 4064
  - id: fader_1
    pin: { cd74hc4051: faders, number: 0 }
    cc: 20

encoders:
  - id: encoder_1
    pin_a: { number: GPIO20, mode: INPUT_PULLUP }
    pin_b: { number: GPIO21, mode: INPUT_PULLUP }
    cc: 30
    steps_per_detent: 4
    acceleration: medium       # off (default) | low | medium | high | { slow, fast, max, curve }
encoder_settings: { acceleration: off }   # the default for encoders without their own

key_matrix:
  type: velocity
  rows: [PA8, PA9, PC0, PC1, PC2, PC3, PC6, PC7]
  columns: [PB0, PB1, PB2, PB3, PB4, PB5, PB8, PB12]
  keys: 32
  first_note: 48
  settle: 1us
  velocity: { fast: 1.6ms, slow: 120ms }
```

**Pins** (ESPHome's pin schema): a string (`PA1`, `GPIO5`, `5`) or a mapping `{ number: PA1, inverted: true, mode: INPUT_PULLUP, allow_other_uses: true }`. On a hub, the chip key names the hub: `{ sn74hc165: buttons, number: 3 }`.
- `mode`: `INPUT`, `OUTPUT`, `ANALOG`, `INPUT_PULLUP`, `INPUT_PULLDOWN` (also `OUTPUT_OPEN_DRAIN`, `INPUT_OUTPUT_OPEN_DRAIN`, which gemifi refuses: no open-drain driver yet), or a mapping of `input`, `output`, `pullup`, `pulldown`, `open_drain` booleans. The default comes from the use: buttons, encoders and matrix columns are inputs, LEDs and matrix rows outputs, analog inputs analog. A mode the use or the chip can't do is an error (an output with a pull-up, a pull-down on NUC1xx, pulls on ESP32 GPIO34–39, a pull-up on a 74HC165 pin). On NUC1xx, `INPUT_PULLUP` is the quasi-bidirectional mode's weak pull-up.
- `allow_other_uses: true`: the pin has more than one use. gemifi only allows that for hub control lines shared between hubs (e.g. one `clock_pin` for an sn74hc595 and an sn74hc165) and for matrix columns shared between a `key_matrix` and `switch_matrix` hubs, with the same job on each and the flag on every use.

**Time periods** (ESPHome style): a number and a unit, `us`, `ms`, `s` (also `ns`, `min`): `scan_interval: 125us`, `delay: 5us`, `settle: 1us`, `velocity: { fast: 1.6ms, slow: 120ms }`. A bare number is an error that says which unit to add.

**Ids and names** (ESPHome rules):
- `id`: what the file refers to (`led:`, `boot_combo:`, the hub key in a pin). A letter, then letters, digits and `_`; not a C/C++ keyword; unique across the whole device (hubs and all components share one namespace). It must be lower_snake_case. Words YAML reads as something else are refused, with the reason: PyYAML is YAML 1.1, so bare `no`, `off`, `On`, `yes`, `true` are booleans, `null` is nothing and `12` is a number; `y`, `n` and quoted `'no'` are refused too, since other YAML tools differ.
- `name`: free text, quoted only when YAML needs it. It is what people see: the row labels of the web config page (the schema's index labels), the CLI's `button.action:Oct -` keys and the debug log. Without it, the name comes from the id: `oct_down` → "Oct down".
- At least one of the two. With only a `name`, the id is derived from it (lowercase, every run of other characters → `_`: "Pitch Down" → `pitch_down`); two derived ids that collide are an error naming both entries.
- Only names reach the firmware (buttons, analog inputs and encoders; LEDs have no name in the tables, since nothing shows one). Ids are a build-time thing and appear only in comments in the generated C.

### Board profiles

On the bare-metal platforms (`stm32f1`, `nuvoton-nuc1xx`) the YAML describes the controls, and a **board profile** describes the board they sit on: `board:` names one of `platform/<platform>/boards/<board>.json` (`python3 tools/builder/gemifi_board.py list`). Left out, it is the platform's first board: `m-audio-code61` on stm32f1, `keystation-mini32-mk3` on nuvoton-nuc1xx, so files from before profiles build as they did.

A profile is data: the crystal (the clock setup is derived from it: on stm32f1 the PLL must make 72 MHz, so 6, 8, 9, 12 or 16 MHz; on the NUC only 12 MHz is known), the flash layout (size, page size, the bootloader's size, the app area, the config-store pages, the update-flag page), the RAM size and the SRAM mailbox, and the pins the board itself uses: a switched USB D+ pull-up, soft power (button and latch), the reset-time recovery combo, an update-mode LED and other reserved pins, each with the reason. The build generates the linker memory regions and the `BOARD_*` layout macros from it (`tools/builder/gemifi_board.py`, `platform/board-profile.mk`), and the builder takes the storage budget's pages and the reserved pins from it: a component on the USB pull-up or a soft-power pin is an error, one that drives a recovery-combo pin a warning.

| Board | `board_id` | Profile |
|---|---|---|
| `m-audio-code61` | 0x0002 (its own) | the vendor layout: app 0x08002400-0x0801AFFF, config pages in the vendor settings area, soft power PC13/PC15, USB pull-up PA8, Up + Down recovery combo |
| `keystation-mini32-mk3` | 0x0001 (its own) | NUC120LC1BN: 32 KB APROM, 4 KB data flash, 4 KB RAM, 12 MHz |
| `generic-stm32f103c8` | the YAML's, 0x3F00-0x3FFF | a Blue Pill / C8 DIY board: 64 KB (52 KB of app), no soft power, no switched pull-up, no recovery combo, the update LED on PC13 (active low). `devices/diy-bluepill.yaml` and `devices/diy-floorboard.yaml` use it |

A vendor board has its own `board_id`, and the YAML's must match it when `board:` is given (without `board:` a mismatch only stops the build). A DIY profile has none: the YAML picks one in the DIY range (protocol/sysex.md §7). Its image's layout isn't in the tools' board table, so the build writes it next to the `.gfw` as `<name>.board.json`; `mkimage.py --check`, `gemifi.py flash` and the web updater (drop both files) check the image against it. For another board, copy `generic-stm32f103c8.json`, change what differs and set `board:` to its name; a board with soft power also needs a board header whose `board_gpio_init()` latches the power at reset.

### Writes the device makes on its own

`write_to_flash:` says what the device may write to its flash **on its own**, without a host asking. A flash write can need a page erase, which stops the CPU, and with it the key scan, for 20-40 ms: one that lands while you play can lose notes or get a velocity wrong. So these writes are off unless the YAML turns them on, and the ones that are on wait until the controller has been left alone.

```yaml
write_to_flash:
  last_preset: true   # remember the selected preset across power cycles (default false)
  delay: 10s          # wait this long after the last change (default 10s, 1s..1h)
  idle: 3s            # and until nothing has been played or touched for this long (default 3s, 100ms..10min)
```

- **`write_to_flash: true` / `false`** is the short form: every item (today only `last_preset`) on or off, with the default timings. Leaving the key out is `false`.
- **`last_preset`**: store `preset.active` after a preset switch, so the device powers up on the preset used last. The write waits until both hold: `delay` has passed since the last switch, and for `idle` no key or button has been held and nothing has been played (keys, buttons, encoders, pads, or an analog input moving enough to send; ADC noise doesn't count). Until then it stays pending, and goes in the next quiet moment. Off (the default): a switch on the device never writes, and the device always powers up on preset 1, whatever an older firmware stored.
- **Host saves always work.** The host's commands write whenever they are sent, whatever this says: COMMIT, PRESET_SAVE, PRESET_NAME, PRESET_DELETE, PRESET_MOVE, PRESET_RESET and FACTORY_RESET (protocol/sysex.md §4.5). They store `preset.count` too. A COMMIT also stores `preset.active` (and clears a pending `last_preset` write), but with `last_preset` off the power-up ignores it.
- `delay` or `idle` with nothing turned on is a warning; so is `last_preset` on a device with presets off (there is nothing to remember, so it stays off).
- **Generated code:** `GM_WRITE_LAST_PRESET` (0/1), `GM_WRITE_DELAY_MS` and `GM_WRITE_IDLE_MS` in `gm_device_config.h`. With `GM_WRITE_LAST_PRESET` 0 the timer and the idle tracking are left out; with 1 the controller keeps a last-input time and a count of keys held (5 bytes of RAM, `gm_idle_ms()`). New items go next to `last_preset` and share `delay` and `idle`.
- **Tests:** `core/test` `test_controller` (the Keystation, off: a switch never writes, a boot ignores a stored `preset.active`) and `test_controller_wtf` (the same YAML with `last_preset: true`: the delay, keys and buttons held, a knob turned, ADC noise, COMMIT), and the NUC host-sim `run_presets.py` (on, with the real journal and reboots).

### Recovery and safety

Three keys, each left out of the firmware when it isn't used.

#### Factory-reset combo

`factory_reset_combo:` lists buttons that, held at power-up, wipe the stored settings: every preset, its name and the globals, as FACTORY_RESET over SysEx does. The firmware and the device ID stay.

```yaml
factory_reset_combo: [oct_down, oct_up]                    # held 5 s (the default)
factory_reset_combo: { combo: [oct_down, oct_up], hold: 8s }   # hold: 1s..30s
```

- The combo counts if every button of it is down within 1.5 s of the start, and stays down for `hold`. Letting go earlier cancels it: nothing is wiped.
- Meanwhile the status LED blinks at 2 Hz and a display shows `RESET?`. Once wiped, the status LED flashes fast for 1.5 s, a display shows `RESET`, `diag.settings` reads 4, and the device carries on with the defaults (no reboot).
- Those buttons do nothing else during the first 1.5 s, or until they are let go when the combo was held. Later on they are ordinary buttons.
- It must differ from `boot_combo`: neither may contain the other, or holding one at power-up would also set off the other (the builder refuses it). Sharing a button is fine. On the Code 61, Up + Down at power-up is the bootloader's, and the YAML's combo uses Octave - + Octave + instead.
- Generated code: `GM_RESET_COMBO` (a button mask, 0 = none) and `GM_RESET_HOLD_MS`.

#### Physical unlock

Opt-in: a device that sets `unlock:` refuses firmware uploads (FW_BEGIN, on A/B platforms), REBOOT to the bootloader and FACTORY_RESET over SysEx with LOCKED until the user OKs them on the device: the device opens a 30 s unlock window, and the user holds the unlock combo for `hold` (protocol/sysex.md §4.9). Chrome asks for SysEx permission once per site, so without this any page that was allowed once could flash or wipe the device.

```yaml
unlock: { combo: [pitch_down, pitch_up], hold: 2s }   # hold: 0s (a press) .. 10s, default 2s
unlock: { hold: 3s }       # the combo defaults to boot_combo
unlock: true               # boot_combo, 2 s
unlock: false              # off, the same as leaving it out
```

- **Off unless the YAML sets it**, even on a device with a `boot_combo`; it is then compiled out (`GM_UNLOCK 0`). `unlock: true` and `unlock: { hold }` take the `boot_combo` buttons, and are an error on a device without one. The shipped Keystation and Code 61 YAMLs leave it off, with a commented-out `# unlock: { combo: [...] }` line to turn it on.
- While the window is open, the status LED blinks at 4 Hz and a display shows `HOLD` and the buttons (scrolling). The combo's buttons do nothing else then: no MIDI, no action, and their release is silent too. Outside the window they are ordinary buttons.
- Once unlocked (the status LED lit for 1 s, `UNLOCKED` on a display), the device stays unlocked until it reboots or no request has come for 5 minutes.
- The bootloader has no lock: entering it takes the boot combo, an unlocked REBOOT, or the boot-loop guard, which only trips when the app keeps failing.
- At most 8 buttons. Generated code: `GM_UNLOCK` (0/1), `GM_UNLOCK_COMBO`, `GM_UNLOCK_N`, `GM_UNLOCK_HOLD_MS` and `GM_UNLOCK_REPLY` (the UNLOCK reply's size, which sizes the reply buffer).

#### Status LED

`status_led: <led id>` picks the LED that shows the blink codes. Without it there are none. While a code shows, it takes over the LED; then the LED goes back to what it showed.

| Pattern | Meaning |
|---|---|
| Fast blink, 4 Hz | The unlock window is open: hold the unlock combo |
| Lit for 1 s | Unlocked |
| Blink, 2 Hz | The factory-reset combo is held; keep holding to wipe the settings |
| Very fast, 10 Hz for 1.5 s | Settings wiped by the factory-reset combo |
| 2 flashes, pause, again | Stored settings were refused at power-up (out of range for this firmware, or a component that is gone): those settings are back at their defaults. Saving once clears it |
| 3 flashes, pause, again | Started after the boot-loop fallback: the bootloader had stopped the app because it kept failing (protocol/sysex.md §6) |
| 4 flashes, pause, again | Started after a watchdog reset or a crash |

The flash codes show for the first 10 s after power-up (200 ms on, 300 ms off per flash, then 1.2 s dark), the most serious one if there are several (3, then 4, then 2). The diagnostics params (`diag.*`, protocol/sysex.md §4.10) say the same, and more, to a host.

Generated code: `GM_STATUS_LED` (the LED index, -1 = none). Tests: `core/test` `test_controller` (the Keystation plus `unlock: true`: unlock, the window, the idle relock, the factory-reset combo, blink code 2, `diag.*`), `test_controller_wtf` (the Keystation as shipped: unlock compiled out, nothing replies LOCKED) and the NUC host-sim `run_presets.py` (its Makefile adds `unlock: true`; FACTORY_RESET locked, then unlocked, also through `gemifi.py`).

**Format 1** files (`gemifi: 1`) are refused with one error per old key, at its line, saying what replaced it (`label:` → `id:`/`name:`, `pin: { pin: PA1 }` → `{ number: PA1 }`, `hubs:` + `type:` → per-chip lists, `pullup: true` → `mode: INPUT_PULLUP`, `shared: true` → `allow_other_uses: true`, `scan_us: 125` → `scan_interval: 125us`, and so on).

**Editors.** The builder makes a JSON schema of this format (`gemifi_build.py schema`), published at https://cubicecho.github.io/gemifi/schema/device.json and with each release. With VS Code's YAML extension, a first line of `# yaml-language-server: $schema=https://cubicecho.github.io/gemifi/schema/device.json` (the files in `devices/` have it) gives completion and marks unknown keys and wrong types. The builder still has the last word: the schema doesn't know which pins a chip has, which ids exist or what a platform can do. For the tags below, add `"yaml.customTags": ["!include scalar", "!include mapping", "!extend scalar", "!remove scalar"]` to the VS Code settings.

### Packages and substitutions

A device file can be built from other files, as in ESPHome. It all happens in the builder (`tools/builder/gemifi_yaml.py`), before validation, so the firmware is the same as if the file had been written out in full.

```yaml
substitutions:
  node: my-keys
  base_cc: 20
packages:
  keys: preset:keystation-mini32-mk3   # devices/keystation-mini32-mk3.yaml
  pads: pads.yaml                      # next to this file
  leds: !include { file: leds.yaml, vars: { first: 4 } }
min_version: 0.34.0
name: ${node}
buttons:
  - id: !extend sustain                # change one of the package's buttons
    cc: ${base_cc}
  - id: !remove oct_up                 # drop one
  - id: extra                          # add one
    pin: PB9
    action: cc_toggle
    cc: 21
scan_interval: !remove                 # drop a key a package set (back to the default)
factory_reset_combo: [oct_down, mod]   # the package's combo named oct_up: a key here replaces it
```

- **`packages:`** is a mapping of name: package, or a list of packages. A package is a part of a device file, merged under this one. It is a file name (relative to the file that names it), `preset:<name>` (`devices/<name>.yaml`), an `!include`, or a mapping written in place. Packages can have packages of their own. Remote packages (`github://...`) aren't supported yet.
- **Merging.** Packages merge in order, then the device file goes on top. Mappings merge key by key. A list whose items all have an id (or a `name:` the id comes from) merges by id: an item with the same id as a package's replaces it in place, other items are added at the end. Any other list replaces the package's. A scalar replaces the package's. Key order is kept (it matters for hubs: their order is the file's).
- **`- id: !extend <id>`** merges the item's other keys into the package's item with that id, instead of replacing it. **`- id: !remove <id>`** drops that item. Both are errors when no package brought that id in. **`key: !remove`** drops a key a package set.
- **`!include file.yaml`** puts that file's content in place of the value, anywhere. `!include { file: f.yaml, vars: { name: value } }` also sets substitutions for that file only.
- **`substitutions:`** is a mapping of name: value (text or a number). `${name}` is replaced in every string value. A value that is only `${name}` takes the substitution's type, so `cc: ${base_cc}` is a number. Substitutions can use other substitutions. Packages can bring substitutions; the device file's win, and `gemifi_build.py validate|generate|build ... -s NAME VALUE` wins over both. An unknown `${name}` is an error. Inside `{ ... }`, quote a value that has `${` in it: `{ cc: "${base_cc}" }` (YAML reads the braces otherwise). Keys and ids from a package are matched before substitutions are made, so `!extend` an id as the package writes it (or give the package's ids through `vars:`).
- **`min_version: x.y.z`** in any of the files: the builder refuses to build with an older version, and says which one is needed. The builder's version is `VERSION` in `tools/builder/gemifi_device.py` (set at each release).
- **Errors** in a package or an included file give that file and its line (`pads.yaml:12: error: ...`). The dashboard shows them with the file's name and doesn't jump to them.
- **The dashboard's "new device from a preset"** (and `gemifi_build.py new`) writes a short file that uses the preset as a package, so it follows the preset when gemifi updates it. `gemifi_build.py new --copy` copies the preset instead.

### Config hash

The builder hashes the resolved config (after packages, includes and substitutions) and bakes it into the firmware as `GM_CONFIG_HASH` in `gm_device_config.h`. The device reports it as the read-only param `config.hash` (id 39, protocol/sysex.md §4.4), so a host can tell which config a device runs. The web page shows it in the device info. With the dashboard, it also says whether the device runs the config of each file with its node name: this config, the file's last build (the file has changed since), or a different config.

- **What is hashed:** the resolved tree as one line of JSON (UTF-8, no spaces): mappings in file order, lists in order, every scalar as `"<type>:<text>"`, e.g. `"int:0x40"`, `"str:Sustain"`, `"bool:true"` (the text as written, after substitutions). Comments, layout, `substitutions:`, `packages:`, `min_version:` and how the config is split into files don't count; two files hash the same when they read the same, key order included. The value is the first 4 bytes of the SHA-256, big-endian, with the top bit cleared (0..0x7FFFFFFF).
- `gemifi_build.py build` prints it (`config hash: 09754273`) and leaves it in `<out>/config.hash`.
- It costs about 80 bytes of flash and 4 of RAM. Firmware built before it has no `config.hash`.

## Presets

A preset is a named set of *behaviours*: what the controls send, not how they are wired (research/core/presets-design.md). The YAML defines them; the user switches them at runtime with the preset button actions or from the editor (`preset.active`).

**Presets off.** A device without `presets:` has presets off: one slot, "Default", that only holds its settings. They are edited live and stored by COMMIT as on any device, but there is nothing to switch: HELLO leaves out caps bit 5, the PRESET_* commands answer BAD_CMD, no PRESET_CHANGED is sent, and the schema has no `preset.*` params, no "Presets" group and no preset button actions (protocol/sysex.md §4.5). The web updater then shows plain settings, with no preset card. The preset actions (`preset_next`, `preset_prev`, `preset_select`, in a button or a mode) are errors that say presets are off. `presets:` turns them on and needs 2 or more slots: one slot would be presets off again, so the builder refuses it.

```yaml
presets:
  slots: 8
  revision: 1
  list:
    - name: Default        # slot 1: the components' own values (above); only name: and program:
    - name: Drums
      program: 9
      midi: { channel: 10, transpose: 0, octave: 0 }   # start values (the octave buttons still move them)
      velocity: { curve: hard, fixed: 100 }
      bend: { glide: 50ms }
      ramp: { time: 300ms }
      buttons:                                         # by id, with the button's own keys
        sustain: { action: note, note: 36 }
        edit:    { note: 38 }                          # action left out: keeps the button's
      analog:   { volume: { cc: 11 } }
      encoders: { encoder_1: { cc: 21, mode: relative_offset } }
      pads: { kick: { note: 35 } }
      pad_settings: { channel: 10, curve: soft, aftertouch: true }
```

Each key, its range and default: [the preset keys](yaml/presets.md#keys).

- **Slot 1** is the first list entry (or "Default" when the list is empty, with `slots:`); its values are the rest of the file. **Other presets** list only what differs from slot 1, by component id; an override equal to slot 1's value is dropped with a warning. **Empty slots** (`slots` minus the listed ones) start as copies of slot 1, named "Preset N", for the user to fill in.
- **Presets in use.** A fresh device has the listed presets in use; the other slots are free, and the preset buttons and keys step through only the ones in use (with one in use, they only reload it). A host makes a new preset in the next free slot (PRESET_NAME or PRESET_SAVE) and deletes one with PRESET_DELETE: the presets after it move up a slot, so the free slots are always last. The count is the read-only global `preset.count`, stored with the globals (protocol/sysex.md §4.5 "Presets in use"). PRESET_MOVE reorders them: a preset moves to another slot and the ones between shift by one.
- **What a preset can set** is the mapping: the keys that start a per-preset param ([the preset keys](yaml/presets.md#keys) lists them with their params; `actions:` gives a button another action list, written as its `button.number`, components/buttons/README.md "Action lists"). The hardware side stays global and is refused in a preset, with where to set it: pins, LEDs, names, a control's `channel`/`min`/`max` and a button's `bank`, `hold`, `double_tap` and `repeat` (gestures are the same in every preset), `adc_min`/`adc_max`, `takeover`, `steps_per_detent`, the key-time calibration (`key_matrix.velocity`), the pad sensing (`pad_settings` `threshold`, `release`, `full`, `window`, `retrigger`), `midi.transport`.
- **Power-up** loads the globals, then preset 1, or with `write_to_flash: { last_preset: true }` the preset used last ("Writes the device makes on its own"). A switch on the device stores nothing unless `last_preset` is on.
- **Storage.** The builder checks that every preset's worst-case stored record (every per-preset value changed, and a name) fits the platform's store, errors with the largest `slots` that fits, and warns above 85% (tools/builder/README.md "Storage budget"). The store keeps records in `include/config_store.h`'s v2 format (2 words a value), except on the STM32F1, which packs them (v3: one group per param, a byte or a halfword a value, `platform/stm32f1/src/config_store.c`) to fit its 3 config pages of 1 KB: there the Code 61 fits up to 5 slots.

**Generated code** (`gm_device.h` `gm_preset_def_t`): `GM_N_PRESETS` (slots; 1 without `presets:`, which `gm_settings.h` `GM_HAS_PRESETS` reads as presets off) and `GM_PRESETS_REVISION` in `gm_device_config.h`, and in `gm_device_config.c` `gm_presets[GM_N_PRESETS]` (slot n = `gm_presets[n - 1]`) with one sorted `{ param id, index, value }` array per preset that has overrides:

```c
static const gm_preset_val_t preset_2_vals[6] = { /* Drums */
    { 1, 0, 10 }, /* midi.channel = 10 */
    { 4, 0, 2 }, /* velocity.curve = hard */
    { 9, 0, 4 }, /* button.action[sustain] = note */
    { 9, 1, 4 }, /* button.action[edit] = note */
    { 10, 0, 36 }, /* button.number[sustain] = 36 */
    { 10, 1, 38 }, /* button.number[edit] = 38 */
};
const gm_preset_def_t gm_presets[GM_N_PRESETS] = {
    { "Default", GM_PROGRAM_NONE, 0, 0 }, /* slot 1 */
    { "Drums", GM_PROGRAM_NONE, 6, preset_2_vals }, /* slot 2 */
    { "Preset 3", GM_PROGRAM_NONE, 0, 0 }, /* slot 3 */
};
```

## DAW feedback

What the host (a DAW) sends back reaches the controller: LEDs follow the DAW's state, toggles and encoders stay in sync with it, knobs don't jump, and a second USB port can speak Mackie Control. Plan and decisions: issue #6.

The MIDI a device accepts and sends for all of this, as implemented: [`protocol/host-feedback.md`](../protocol/host-feedback.md). Per-DAW setup: the site's "DAW setup" page (`site/src/daw-setup.md`).

**Incoming MIDI** (`src/midi_in.c`). The platform hands over USB-MIDI event packets (`gm_midi_rx_usb`, NUC and ESP32-S2/S3 USB: the cable is kept) or a byte stream (`gm_midi_rx_byte`, serial and BLE: one port, running status handled). SysEx on the main port (cable 0) goes to the gemifi protocol exactly as before; Note On/Off, CC, pitch bend, channel pressure and realtime (clock `F8`, start `FA`, continue `FB`, stop `FC`) go to the feedback code, and on the DAW port (cable 1) to Mackie Control. `feedback: { enabled: false }` without `mcu:` compiles all of that out (SysEx only, as before).

### Plain feedback (F1, every DAW)

Works with any host that sends MIDI back to the controller's port. In Ableton Live: Preferences → Link, Tempo & MIDI → turn on **Remote** for the controller's *output* port; Live then sends the value of every parameter mapped to one of our CCs/notes when it changes. For clock and transport LEDs, also turn on **Sync** for that output.

Messages count on the device's MIDI channel (`midi.channel`) unless an LED names one.

| On the device | What the host's message does | YAML |
|---|---|---|
| `cc_toggle` button | its CC sets the toggle state (≥ 64 = on), so its LED and the next press agree with the DAW | nothing: always on |
| `cc_momentary` / `cc_ramp` / `note` button with an LED | its CC / note lights the LED | nothing (see "LED ownership") |
| LED | follows a note, a CC, the beat or the transport | `feedback:` on the LED |
| encoder, mode absolute | its CC becomes the encoder's value: the next turn continues from there | nothing: always on |
| analog | pick-up: after the host sets a value, the knob is silent until it reaches or crosses it | `takeover: pickup` |

```yaml
feedback:                     # optional; these are the defaults
  enabled: true               # false: ignore everything but the gemifi SysEx protocol (and MCU, if mcu: is given); also turns off DAW scripts (F3)
  led_owner: host_then_local  # default for every LED: host_then_local | host | local
  led_timeout: 2s             # host_then_local: how long the host's last word lasts (a period, or never)

leds:
  - id: arm_led
    pin: PC4
    feedback: { note: 40, channel: 16 }    # velocity 0 off, 1 blinking, 2..127 on
  - id: mute_led
    pin: PC8
    feedback: { cc: 90 }                   # value 0 off, 1 blinking, 2..127 on; channel: midi.channel
  - id: beat_led
    pin: PC5
    feedback: { clock: beat }              # lit for the first 16th of every beat (MIDI clock)
  - id: play_led
    pin: PC6
    feedback: { transport: playing }       # Start / Continue on, Stop off
  - id: sustain_led
    pin: PA12
    owner: host                            # the host only (LED off until it sends something)
    timeout: never                         # (only used with host_then_local)

analog:
  - { id: volume, pin: PA0, cc: 7, takeover: pickup }   # none (default) | pickup
```

LED `feedback:` takes exactly one of `note:` / `cc:` (a number 0..127, with an optional `channel: 1..16`), `clock: beat`, `transport: playing`, or `mcu: <MCU LED>` (below). Values map to the LED as in Mackie Control: 0 off, 1 blinking, anything else on (the first colour of a bicolour LED). DAW scripts (F3) can also reach a bicolour LED's second colour, and set an RGB LED's colour (FB_LED_RGB). An RGB LED (`pin3:`) shows its active look (white unless `active:` says otherwise) for any level from its button, plain feedback, Mackie Control or FB_LED, and its idle look for 0 ([leds "LED looks"](../components/leds/README.md#led-looks-idle-and-active)); only FB_LED_RGB picks a colour: an LED of an LED strip (`led_strip:`) shows it as given (each channel 0..127 sent as 0..255), any other RGB LED with each channel on or off (on if it is at least half the brightest channel: 127/60/0 is red and green).

### LED ownership

An LED can have a local state (its button: held, toggled, the octave, ...) and a host state (the host's last message for it). `owner:` decides which one shows; `feedback.led_owner` sets the default for every LED:

| `owner:` | Shows |
|---|---|
| `host_then_local` (default) | the host's state for `timeout` (default 2s, `feedback.led_timeout`) after the host's last message for it, then the local state again. `timeout: never`: the host's state from its first message on |
| `host` | only the host's state (off until the host sends something) |
| `local` | only the local state; the host's messages are ignored |

An LED with no local state (no button uses it) shows the host's state with `host_then_local` or `host`; with `owner: local` it never lights, so the builder warns about that. LEDs driven by Mackie Control (an `action: mcu` button's LED, `feedback: { mcu: ... }`, ring LEDs) default to `host`, since MCU hosts only send LED changes. The default timeout is a question in #6.

### Second USB port and Mackie Control

`mcu:` turns on Mackie Control (MCU, written from community documentation of the protocol; "Mackie Control" names only what it's compatible with). On USB the device then gets a second MIDI port, `<friendly_name> DAW` (USB-MIDI cable 1 on the same endpoints; `usb: { daw_port: true }`, the default with `mcu:`), and MCU lives there, so keys and knobs on the main port are never read as MCU buttons. Without `mcu:` the USB descriptor is unchanged, byte for byte. On serial and BLE there is one port: `mcu:` makes the whole device speak MCU (plain feedback is then off, and the builder refuses `feedback:` sources that need it).

In the DAW: Live: Control Surface "MackieControl", input and output = the DAW port. Bitwig, REAPER, Logic, Cubase: their "Mackie Control" surface on the DAW port.

```yaml
mcu:
  extender: false                 # true: answer as an MCU XT (SysEx device id 0x15 instead of 0x14)

buttons:
  - { id: play, pin: PB0, mcu: play, led: play_led }        # action: mcu (implied); Note On 0x5E; LED from the host
  - { id: rec_1, pin: PB1, mcu: rec_1, led: rec_1_led }     # 0x00
leds:
  - { id: play_led, pin: PC0 }
  - { id: rec_1_led, pin: PC1 }
  - { id: record_led, pin: PC2, feedback: { mcu: record } } # an MCU LED without a button
  - { id: r0, pin: PD0 }
  - { id: r1, pin: PD1 }
  - { id: r2, pin: PD2 }
analog:
  - { id: fader_1, pin: { cd74hc4051: faders, number: 0 }, mcu: fader_1 }   # pitch bend ch 1; takeover: pickup by default
encoders:
  - { id: pan_1, pin_a: GPIO20, pin_b: GPIO21, mcu: vpot_1, ring: [r0, r1, r2], ring_center: rc }   # CC 0x10 relative; ring from CC 0x30
```

| YAML | MCU message |
|---|---|
| button `mcu:` / LED `feedback: { mcu: }` | Note On ch 1: `rec_1`–`rec_8` 0x00–07, `solo_1`–`8` 0x08–0F, `mute_1`–`8` 0x10–17, `select_1`–`8` 0x18–1F, `vpot_push_1`–`8` 0x20–27, `track` `send` `pan` `plugin` `eq` `instrument` 0x28–2D, `bank_left` `bank_right` `channel_left` `channel_right` 0x2E–31, `flip` `global` `name_value` `smpte_beats` 0x32–35, `f1`–`f8` 0x36–3D, `midi_tracks` `inputs` `audio_tracks` `audio_instruments` `aux` `busses` `outputs` `user` 0x3E–45, `shift` `option` `control` `alt` 0x46–49, `read` `write` `trim` `touch` `latch` `group` 0x4A–4F, `save` `undo` `cancel` `enter` 0x50–53, `marker` `nudge` `cycle` `drop` `replace` `click` `solo` 0x54–5A, `rewind` `fast_forward` `stop` `play` `record` 0x5B–5F, `up` `down` `left` `right` `zoom` `scrub` 0x60–65, `user_a` `user_b` 0x66–67, `fader_touch_1`–`8` 0x68–6F, `master_touch` 0x70, `smpte_led` `beats_led` `rude_solo_led` 0x71–73, `relay` 0x76. Press 7F, release 00; the host's 00 / 01 / 7F = off / blinking / on |
| analog `mcu: fader_1`–`fader_8`, `master` | pitch bend ch 1–8 / 9, 14 bit, both ways (no motors: the host's position is the pick-up target) |
| encoder `mcu: vpot_1`–`vpot_8` | CC 0x10–0x17: 01–0F = +n, 41–4F = −n |
| encoder `ring:` (1–16 LED ids, leftmost first) | CC 0x30–0x37 from the host: position 1–11 spread over the LEDs, modes dot / boost-cut / wrap / spread |
| encoder `ring_center:` (an LED id) | bit 6 of the same CC: the LED under the V-Pot |
| (to hooks) | CC 0x40–0x4B: the 12 timecode / assignment characters (`gm_mcu_char()`, `gm_mcu_on_char`); SysEx `12`: LCD text (`gm_mcu_on_lcd`, streamed in pieces); channel pressure: meter levels (`gm_mcu_on_meter`); SysEx `20`: meter modes (`gm_mcu_meter_mode()`). A device with a display or meter LEDs shows them; nothing does yet |
| (automatic) | SysEx `00` device query → `01` serial + challenge; the host's `02` answer → `03` (accepted without checking it, so no response algorithm is needed); `61` faders to minimum (pick-up targets), `62` all MCU LEDs off, `63` reset |
| (automatic) | the version request `F0 00 00 66 14 13 00 F7` gets `… 14 14 "V0.01" F7` (`V<major>.<minor, 2 digits>` from the firmware version, so 0.1.x → `V0.01`), which is all Live asks |

Not done: fader touch notes (needs touch-sensing faders), extender chaining beyond one device id, and showing the display and meter hooks on real hardware (needs a display component). See #18.

**Cost** (NUC120, Keystation config): plain feedback (the default) adds about 0.5 KB of flash and 24 B of RAM; `feedback: { enabled: false }` leaves 60 B (the packet dispatcher); `mcu:` adds about 0.5 KB more. Each LED with a host state uses 5 B of RAM; per-LED settings, pick-up and MCU tables are only generated when the YAML uses them.

**Finding the components** (`feedback.c`). A note or CC on the device channel goes to the buttons, analog inputs and encoders (twins included) with its number. Up to 32 of them, the device walks them all, after a 32-byte bitmap of the numbers in use has let it skip the numbers nobody has. With more (the Code 61's 103), it keeps an index instead: per number its first component and a chain to the next, 128 B + 1 B per component (Code 61: +200 B of RAM against the bitmap), so a used CC costs ~60 cycles instead of ~1,100. Either is rebuilt in the main loop on the first message after a param changes (a host SET, a preset switch); layers and cycles only pick the live twin, and feedback reaches every twin. `-DGM_FB_INDEX=0` or `1` overrides the rule.

### DAW scripts (F3)

A script inside the DAW (`hosts/`, e.g. the Ableton Live Remote Script) can drive the device directly, by component index rather than by note or CC: it asks the device for its components (GET_COMPONENT), hears presses through EVENT messages, and sets LEDs, values, rings and text with fire-and-forget messages (`protocol/sysex.md` §4.6). GET_COMPONENT also says whether FB_VALUE and FB_LED will do anything: an analog input's `takeover`, an encoder's mode and an LED's `owner`. About 1.5 s after it starts, the device sends one BOOTED message, so a running script knows its states are gone and sends them again. It's on by default and costs about 1.5 KB of flash on the Keystation (most of it the component names).

```yaml
feedback:
  script: true                  # default; false leaves GET_COMPONENT and FB_* out

buttons:
  - { id: rec, pin: PB0, action: cc_momentary, cc: 20, led: rec_led, daw: record }
analog:
  - { id: vol, pin: PA0, cc: 7, takeover: pickup, daw: track_volume }
encoders:
  - { id: p1, pin_a: PB2, pin_b: PB3, cc: 30, daw: device_param_1, ring: [r0, r1, r2, r3, r4] }
```

`daw:` tells a script what to connect the component to; the names are in `hosts/README.md` and `protocol/daw-scripts.md`, `x_<name>` is your own, and `<host>.<command>` (lower case, e.g. `zynthian.toggle_audio_play`) one host's command; a role is at most 31 characters. Displays take `daw:` too (`time`, `track_name`, `param`, `status`). Without `daw:`, a component's `mcu:` name is its role (`channel_left` / `channel_right` as `strip_prev` / `strip_next`).

**Touch.** `touch: movement` on an analog input makes it say when it is touched, for a script that treats a touched fader differently (automation touch, a value shown while it moves): EVENT type 5 (`debug.events` on), the analog index, value 1 just before the first value EVENT of a movement, and 0 once the value has been still for 300 ms. GET_COMPONENT's analog `touch` byte (after `takeover`) is 0 none, 2 movement; 1 (a touch sensor, `touch: { pin: ... }`) is reserved and the builder refuses it for now. With scripts on, `ring:` and `ring_center:` work without `mcu:`. A script's LED states don't time out (`led_timeout` doesn't apply) and `owner: local` LEDs ignore them; the script hands them back when it stops.

## Generated code (tools/builder output)

`gm_device_config.h`:
```c
#define GM_FW_VERSION 0x00010003u      /* from project.version (major << 24 | minor << 16 | patch) */
#define GM_FW_VERSION_STR "0.1.3"      /* the NUC Makefile reads this for the .gfw header */
#define GM_N_BUTTONS 7
#define GM_N_LEDS 4
#define GM_N_ANALOGS 1
#define GM_N_ENCODERS 0
#define GM_N_HUBS 0
#define GM_HAS_MATRIX 1
#define GM_MATRIX_KEYS 32
#define GM_MATRIX_VELOCITY 1
#define GM_MATRIX_ROWS 8                  /* key-matrix rows scanned (at most 32): sizes the scan's per-row tables */
#define GM_MATRIX_COL_RUNS 3              /* runs of adjacent column bits on one port (PB0-5, PB8, PB12) */
#define GM_MATRIX_DECODER 0               /* 1: a row is a decoder output (the one-store row selects are kept) */
#define GM_BOARD_ID 0x0001
#define GM_DEVICE_NAME "Keystation Mini 32"   /* friendly_name */
#define GM_NODE_NAME "keystation-mini32"       /* name: sent at the end of HELLO (sysex.md 4.1.1) */
#define GM_USB_VID 0x1209
#define GM_USB_PID 0x0002
#define GM_HAS_I2C 0
#define GM_PLATFORM "nuvoton-nuc1xx"
/* host feedback / DAW ("DAW feedback") */
#define GM_FEEDBACK 1                     /* F1 on the main port (feedback: enabled) */
#define GM_LED_FB 0                       /* 1: gm_led_fb[] has per-LED sources / owners */
#define GM_LED_OWNER 0                    /* feedback.led_owner: 0 host_then_local, 1 host, 2 local */
#define GM_LED_TIMEOUT 200                /* feedback.led_timeout, 10 ms units (0xFFFF = never) */
#define GM_ANALOG_PICKUP 0                /* 1: gm_analog_pickup[] */
#define GM_ANALOG_TOUCH 0                 /* 1: gm_analog_touch[] (analog touch:) */
#define GM_MCU 0                          /* 1: mcu: (then also GM_MCU_CABLE, GM_MCU_ID) */
#define GM_USB_DAW_PORT 0                 /* 1: second USB-MIDI cable (then also GM_DAW_PORT_NAME) */
/* write_to_flash ("Writes the device makes on its own") */
#define GM_ENC_ACCEL 0                    /* 1: gm_enc_accel[] (encoder acceleration:) */
#define GM_MSG_CC14 0                     /* "MIDI messages": the kinds compiled in (also GM_MSG_NRPN, */
#define GM_N_SYSEX 0                      /* _PROGRAM, _PANIC, _MMC; GM_BTN_CTL, ... for the tables) */
#define GM_WRITE_LAST_PRESET 0            /* 1: store preset.active on its own once quiet */
#define GM_WRITE_DELAY_MS 10000u          /* write_to_flash.delay */
#define GM_WRITE_IDLE_MS 3000u            /* write_to_flash.idle */
#define GM_UNLOCK 1                       /* physical unlock (unlock:, opt-in; here unlock: true) */
#define GM_UNLOCK_COMBO 0x30u             /* pitch_down + pitch_up (bit = button index) */
#define GM_UNLOCK_N 2
#define GM_UNLOCK_HOLD_MS 2000
#define GM_UNLOCK_REPLY 25                /* UNLOCK reply payload bytes */
#define GM_RESET_COMBO 0xCu               /* factory_reset_combo: oct_down + oct_up; 0 = none */
#define GM_RESET_HOLD_MS 5000
#define GM_STATUS_LED 1                   /* status_led: edit_led; (-1) = none */
/* ESP only: */
#define GM_MIDI_TRANSPORT GM_MIDI_USB     /* GM_MIDI_USB / _SERIAL / _BLE, values in gm_hal.h */
#define GM_I2C_SDA 8                      /* GPIO numbers, when i2c: is given; */
#define GM_I2C_SCL 9                      /* otherwise the board's default Wire pins */
```
Optional build flags on ESP: `GM_MIDI_BAUD` (serial transport, default 115200), `GM_FW_VERSION` (`major << 24 | minor << 16 | patch`; normally set by the YAML `project: { version }`).
`gm_device_config.c`: `const gm_device_t gm_device = {...}` with its tables, and the default arrays `gm_button_action_defaults[]`, `gm_button_number_defaults[]`, `gm_analog_cc_defaults[]`, `gm_encoder_cc_defaults[]` (each at least one element long, so a count of 0 still compiles). Only when the YAML uses them: `gm_led_fb[]` (`GM_LED_FB`), `gm_analog_pickup[]` (`GM_ANALOG_PICKUP`), `gm_analog_mcu[]`, `gm_encoder_mcu[]`, `gm_encoder_ring[]` (`GM_MCU`), `gm_enc_accel[]` (`GM_ENC_ACCEL`, [encoders "Encoder acceleration"](../components/encoders/README.md#encoder-acceleration)), the "MIDI messages" tables (`gm_button_ctl[]`, `gm_analog_ctl[]`, `gm_encoder_ctl[]`, `gm_button_bank[]`, `gm_encoder_mode_defaults[]`, `gm_sysex[]`). Plain `make` (firmware and `core/test`) generates them from `devices/keystation-mini32-mk3.yaml` into the build dir.
