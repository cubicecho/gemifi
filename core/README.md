# gemifi core

Chip-independent C code shared by every gemifi firmware: the SysEx protocol, the config profile, and the generic MIDI controller runtime. A platform (`platform/<family>/`) adds a small HAL; a device YAML (`devices/*.yaml`) describes the hardware; `tools/builder` turns the YAML into C tables and builds.

```
devices/<device>.yaml ──tools/builder──▶ gm_device_config.h / .c ──┐
core/  (protocol, params, controller, virtual pins)                ├─▶ firmware image
platform/<family>/  (HAL: pins, ADC, timer, MIDI transport, storage, reboot, upload)
```

## Files

| File | What |
|---|---|
| `include/gsx.h`, `src/gsx.c` | SysEx frame codec (`protocol/sysex.md` §1–3) |
| `include/params.h`, `src/params.c` | param table, GET/SET validation, schema blob (§4.3, §4.4) |
| `include/config_store.h` | stored-profile API; implemented per platform |
| `include/app_proto.h`, `src/app_proto.c` | app-side request handling: HELLO, REBOOT, identity, config, FW_* (via `hal_fw_ops`), LOG/EVENT/PARAM_CHANGED |
| `include/gm_device.h` | the hardware model: virtual pins, hubs, buttons, LEDs, analog inputs, encoders, key matrix, pads |
| `include/gm_hal.h` | what a platform provides |
| `include/gm_controller.h`, `src/controller.c` | the runtime: scanning, actions, velocity, LEDs, MIDI out, host feedback (F1) |
| `src/midi_in.c` | incoming MIDI: USB-MIDI packets or a byte stream, sorted by port into SysEx (the protocol), feedback and MCU |
| `include/gm_feedback.h` | the feedback switches (`GM_FEEDBACK`, `GM_MCU`, ...) and the calls between the files above |
| `include/gm_mcu.h`, `src/mcu.c` | Mackie Control on the DAW port; empty unless the YAML has `mcu:` |
| `src/vpin.c` | virtual pins: MCU pins via the HAL, expander hubs in software |
| `src/settings.c` | the param table, sized from the device's component counts |

Rules: C11, no heap, no floating point, no libc beyond `<stdint.h>` (the NUC120 has 4 KB of RAM and 32 KB of flash). Headers are C++-safe (the ESP platform is Arduino C++).

## Virtual pins and hubs

Every pin in a device is a `gm_pin_t { hub, flags, num }`:
- `hub = 0`: a pin of the MCU. `num` is the platform's pin number: NUC1xx `port * 16 + bit` (`PA0` = 0, `PB12` = 28, `PC7` = 39); ESP32/ESP8266 the GPIO number.
- `hub = n ≥ 1`: pin `num` of hub `n - 1` (chained shift registers continue: pins 8–15 are on the second chip). Hubs are numbered in the order they appear in the YAML.
- `flags`: `GM_PIN_INVERT` (YAML `inverted: true`: active-low, pressed / lit = electrical 0), `GM_PIN_PULLUP`, `GM_PIN_PULLDOWN` (YAML `mode: INPUT_PULLUP` / `INPUT_PULLDOWN`; MCU pins, and pull-ups on MCP23017 pins), `GM_PIN_OPEN_DRAIN` (YAML `mode: OUTPUT_OPEN_DRAIN`; only a `tm1628` `dio_pin`, platform mode `HAL_OUT_OD`).

Hubs are declared ESPHome style, one top-level list per chip, named like the ESPHome component:

| Chip key (YAML) | `gm_device.h` | Pins it uses | Gives |
|---|---|---|---|
| `sn74hc595` | `GM_HUB_HC595` | `data_pin` (SER), `clock_pin` (SRCLK), `latch_pin` (RCLK), optional `oe_pin` (/OE); `sr_count` chips (1–4) | 8 × sr_count outputs. LEDs, matrix rows |
| `sn74hc165` | `GM_HUB_HC165` | `data_pin` (QH), `clock_pin` (CLK), `load_pin` (SH/LD); `sr_count` chips (1–4) | 8 × sr_count inputs, numbered as ESPHome does: 0–7 on the chip whose QH goes to the MCU (0 = its A), 8–15 on the next. Buttons, matrix columns |
| `cd74hc4051` | `GM_HUB_MUX` | `pin_s0`–`pin_s2`, `common_pin`, optional `enable_pin` (/E), `delay` (settle time after switching channel, default 2us) | 8 input channels, analog (if `common_pin` is an ADC pin) or digital. Muxes may share select lines (`allow_other_uses`); give them the same `delay` |
| `cd74hc4067` | `GM_HUB_MUX` | `pin_s0`–`pin_s3`, `common_pin`, optional `enable_pin`, `delay` | 16 input channels, as above |
| `mcp23017` | `GM_HUB_MCP23017` | I2C (`address`, default 0x20; the `i2c:` pins) | 16 GPIO (`0`–`15` or `GPA0`–`GPB7`) |
| `pcf8574` | `GM_HUB_PCF8574` | I2C (`address`) | 8 GPIO (`0`–`7` or `P0`–`P7`; quasi-bidirectional: inputs are written high) |
| `decoder` | `GM_HUB_DECODER` | `address` (1–4 pins, A0 first), optional `groups` (1–4 mappings of enable pin to level, e.g. `{ PB6: 1, PB7: 0 }`) | One-of-N row selects (74HC138-style chips sharing address lines): output n = address n % 2^address-pins in group n / 2^address-pins. Only matrix rows (`key_matrix`, `switch_matrix`) |
| `switch_matrix` | `GM_HUB_SWMATRIX` | `rows` (MCU pins or decoder outputs), `columns` (MCU input pins), `settle` (wait after driving a row, default 1us) | Up to 32 switches, scanned from the scan interrupt: pin n = row n / columns, column n % columns, 1 = closed. Buttons, encoders |
| `rgb_serial` | `GM_HUB_RGB_SERIAL` | `tx_pin` (stm32f1: PA2 only); `count` (1–32 LEDs, default 16) | 3 × count outputs: pin = LED × 3 + colour (0 red, 1 green, 2 blue). See "RGB serial LEDs" |
| `tm1628` | `GM_HUB_TM1628` | `clk_pin`, `dio_pin` (may be `OUTPUT_OPEN_DRAIN`), `stb_pin`; `ram_bytes` (1–16, default 14), `intensity` (0–7, default 7), optional `grids` (4–7) | 8 × ram_bytes outputs: display RAM bits. LEDs (see "TM1628 LED drivers") |

A pin on a hub names the hub by its chip key and its id: `{ sn74hc595: leds, number: 3 }`, `{ cd74hc4051: faders, number: 0 }`. The MCP23017's pin key is `mcp23xxx` (as in ESPHome): `{ mcp23xxx: io, number: GPB7 }`.

Inputs on HC165, MCP23017 and PCF8574 are read once per scan into a cache, and outputs on HC595 are written once per scan, so a whole key matrix can live on shift registers.

**Matrices behind decoders** (the M-Audio Code 61, `devices/m-audio-code61.yaml`): rows come from 3-to-8 decoders that share address lines, and the keys, panel buttons and encoders all share the same columns. Declare the decoder once, then give each matrix a range of its outputs, `rows: { decoder: rows, first: 16, count: 4 }` (or a list of `{ decoder: rows, number: n }` pins). The key matrix and the switch matrices are scanned one after the other from the scan interrupt, so they can share column pins: set `allow_other_uses: true` on each use. A decoder output is selected when its row is driven and stays selected until the next row's select replaces it.

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

### TM1628 LED drivers

`tm1628` covers the TM1628 and its relatives that take the same commands (TM1638, TM1620, …: set `ram_bytes` to what the chip has or what the board uses). They're output-only here: the key-scan inputs aren't read.

- **Pin numbers** are display RAM bits: `number = byte * 8 + bit`, bit 0 = the LSB of the byte. `{ tm1628: leds_a, number: 37 }` is byte 4 (address 0xC4), bit 5. On a TM1628 in its usual wiring, byte 2g is SEG1–SEG8 of grid g+1 and byte 2g+1 is SEG9–SEG14, but the board decides what each bit lights.
- **Sharing**: several chips can share `clk_pin` and `dio_pin` (`allow_other_uses: true` on each), each with its own `stb_pin`. TM1628 lines can't be shared with other hubs' lines.
- **Writing**: `vpin_write` only changes a RAM copy (`GM_TM1628_RAM` bytes in `vpin.c`) and marks the chip dirty if a bit changed. The main loop writes dirty chips after each LED update (every 10 ms), never the scan interrupt: a 14-byte chip is about 150 clocks at 1 µs per half clock, roughly 0.3 ms. A write is: the display-mode command (`grids - 4`, only if `grids:` is set), 0x40 (write, auto-increment), 0xC0 and all `ram_bytes` bytes, then 0x88 | `intensity` (display on). Bits go LSB first, DIO set while CLK is low and taken on the rising edge; the bus idles with STB, CLK and DIO high. Each chip is written once at start-up (all off after the lamp test), so it's cleared even with no LEDs on it.
- **Brightness** is per chip (`intensity`, the chip's PWM). LEDs on it are on/off, like other LEDs.

```yaml
tm1628:
  - id: leds_a
    clk_pin: { number: PC10, allow_other_uses: true }
    dio_pin: { number: PC11, mode: OUTPUT_OPEN_DRAIN, allow_other_uses: true }
    stb_pin: PA15
  - id: leds_b
    clk_pin: { number: PC10, allow_other_uses: true }
    dio_pin: { number: PC11, mode: OUTPUT_OPEN_DRAIN, allow_other_uses: true }
    stb_pin: PC12
    ram_bytes: 6
leds:
  - { id: shift_led, pin: { tm1628: leds_a, number: 37 } }
  - { id: pad_1, pin: { tm1628: leds_b, number: 0 }, pin2: { tm1628: leds_b, number: 1 }, pin3: { tm1628: leds_b, number: 2 } }
```

**Finding which bit lights what** (a board whose map is unknown): make a test YAML with one LED per bit, with no buttons using them, so the host drives them all:

```yaml
leds:
  - { id: a_0, pin: { tm1628: leds_a, number: 0 }, feedback: { note: 0 } }
  - { id: a_1, pin: { tm1628: leds_a, number: 1 }, feedback: { note: 1 } }
  # ... one per bit; the second chip on channel 2: feedback: { note: 0, channel: 2 }
```

Build and upload it, then light one bit at a time: from the web app's host-feedback card (each LED is listed by name, with Off / Blink / On), or by sending Note On to the LED's note from any MIDI tool. Note which LED or segment lights for each bit; then replace the list with the real LEDs. The lamp test at start-up lights every listed bit for 0.2 s.

### RGB serial LEDs

Some boards drive their RGB LEDs from a second small controller over a serial line. The one supported is the M-Audio Code pad LED board (Code 61: USART2 TX on PA2, 115200 baud 8N1), found in the stock firmware: one message per LED, `02, 0x80 + LED, R, G, B, 03` (each colour 00 off to FF full), and a lone `A5` keepalive at least every ~60 ms, without which the board turns its LEDs off. The board drops messages sent back to back, so the core sends one changed LED per 5 ms from the main loop (16 LEDs all changing take 80 ms) and the keepalive every 50 ms. The UART runs in the background (DMA on stm32f1, `hal_serial_write`).

```yaml
rgb_serial:
  - { id: pad_leds, tx_pin: PA2, count: 16 }
leds:
  - { id: pad_1_led, pin: { rgb_serial: pad_leds, number: 36 }, pin2: { rgb_serial: pad_leds, number: 37 }, pin3: { rgb_serial: pad_leds, number: 38 } }
pads:
  - { id: pad_1, pin: { cd74hc4051: pad_mux_b, number: 4 }, note: 36, led: pad_1_led }   # lit while held
```

Colours are on or off for now: an RGB LED shows white from its pad or button, or the colour a DAW script picks (FB_LED_RGB).

### LED walk (finding LEDs)

To find which output drives which LED on new hardware, list every candidate output under `leds:` and build with `GM_LED_WALK_MS` set (stm32f1: `make LED_WALK=100`). Each LED then lights on its own for that many milliseconds, in `leds:` order, and the device log shows `walk LED <n>`. Holding any button pauses the walk. Normal LED behaviour is off in such a build; without the define the code isn't built at all.

## Components

Every component has an `id` (what the rest of the file uses to refer to it) and/or a `name` (display text). See "Ids and names" below.

| Component | Fields | Params it creates (per item, as arrays) |
|---|---|---|
| button | `id`/`name`, `pin`, `led` (an LED id, optional), default `action` and its `cc:` (CC actions), `note:` (action `note`) or `mcu:` (action `mcu`) | `button.action`, `button.number` |
| LED | `id`/`name`, `pin`, optional `pin2` (bicolour: the second colour), optional `pin3` (with `pin2`: an RGB LED, `pin` red, `pin2` green, `pin3` blue), optional `feedback:`, `owner:`, `timeout:` ("DAW feedback") | – (LEDs show their button's state, or the octave/transpose level, or what the host sends) |
| analog (pot, slider, fader, expression) | `id`/`name`, `pin` (ADC pin or mux channel), `adc_min`, `adc_max` (the raw 0..4095 reading at the bottom and top; swap them to reverse), default `cc`, optional `takeover:`, `mcu:` | `analog.cc` |
| pad (velocity pad: pressure / FSR) | `id`/`name`, `pin` (ADC pin or mux channel; `inverted: true` if pressure lowers the reading), default `note`, optional `led` (lit while the pad is held); what all pads share goes in the top-level `pad_settings:` ("Pads" below) | `pad.note`, and once per device `pad.channel`, `pad.curve`, `pad.aftertouch`, `pad.threshold`, `pad.release`, `pad.full`, `pad.window`, `pad.retrigger` |
| encoder | `id`/`name`, `pin_a`, `pin_b`, `steps_per_detent`, default `cc`, optional `mcu:`, `ring:` | `encoder.cc`, `encoder.mode` |
| key matrix (at most one) | `rows`, `columns`, `type` (`switch` / `velocity`), `keys`, `first_note`, `settle` (time after driving a row, default 1us), and for `type: velocity` optional default key times `velocity: { fast, slow }` (time between the two contacts for velocity 127 and 1; default 1.2ms and 120ms) | the velocity params if `type: velocity` |

Global params (always present): `midi.channel`, `midi.transpose`, `midi.octave`, `bend.glide`, `ramp.time`, `debug.events`, `debug.log_level`. The list with ids is in `src/settings.c`; ids never change meaning.

**Key matrix.** One row is driven at a time (to the active level: high, or low with `inverted`), the columns are read, then the row is released. `type: switch`: key = row + rows × column. `type: velocity`: two contacts per key; key k is on row k % rows and columns 2p, 2p+1 with p = k / rows. Whichever contact closes first starts the timer, the second sends Note On with a velocity from the time between them, and the key is released when both are open. Put diodes on the matrix if more than one key can be held.

**Button actions** (`button.action` values; YAML names in brackets): 0 none (`none`), 1 CC momentary (`cc_momentary`), 2 CC toggle (`cc_toggle`), 3 CC ramp (`cc_ramp`: glides to 127 while held, back to 0 on release), 4 note (`note`), 5 octave down (`octave_down`), 6 octave up (`octave_up`), 7 transpose down (`transpose_down`), 8 transpose up (`transpose_up`), 9 pitch bend down (`bend_down`), 10 pitch bend up (`bend_up`), 11 program next (`program_next`), 12 program previous (`program_prev`), and on devices with 2 or more preset slots (see "Presets") 13 next preset (`preset_next`), 14 previous preset (`preset_prev`), 15 select preset (`preset_select`: `button.number` is the slot, 1..slots, from the YAML's `preset:`, e.g. `{ action: preset_select, preset: 3 }`), and on devices with `mcu:` only, 16 Mackie Control (`mcu`; it was 13 before presets, so a profile stored with it needs setting again). `button.number` is the CC or note number: the YAML's `cc:` for the CC actions, `note:` for `note`, the slot for `preset_select`, the MCU note for `mcu` (from `mcu: play` etc.). Other actions take neither (a `cc:`/`note:` there only sets the default number for when the action is changed in the config, with a warning).

**Encoder modes** (`encoder.mode`): 0 absolute (0..127, sends the value), 1 relative two's complement (1 = +1, 127 = −1), 2 relative offset (65 = +1, 63 = −1).

## Pads

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
  threshold: 200               # reading that starts a hit, 1..4095       (pad.threshold, global)
  release: 100                 # the pad is let go below this             (pad.release, global)
  full: 3000                   # peak reading for velocity 127            (pad.full, global)
  window: 3ms                  # 1..50ms: peak search after the threshold (pad.window, global)
  retrigger: 20ms              # 0..500ms: no new hit after a release     (pad.retrigger, global)
```

- **Sampling.** Every pad is read once a millisecond from the main loop, in the same context as the analog inputs: they often share a mux's select lines and the ADC, so reading pads from the scan interrupt would race with the analog reads. Pads on muxes are read sorted by channel, so muxes with the same select lines switch them once per channel (the Code 61: 8 switches for 16 pads). On an STM32F1 a 16-pad scan takes about 0.15 ms (16 conversions of ~6 us plus 8 settles of 10 us).
- **A hit.** Readings are 0..4095 (after `inverted`). When a pad reaches `pad.threshold`, its peak over the next `pad.window` ms is taken (4 readings at 3 ms) and Note On goes out with velocity 1..127, linear from `threshold` to `full`, then shaped by `pad.curve` like the keys' curves (soft, hard; fixed = `velocity.fixed`). A pad let go within the window plays at once with the peak so far. It is released (Note Off) below `pad.release`, and can't start a new hit for `pad.retrigger` ms after that (bounce, double hits). The thresholds are live params: tune them from the web app.
- **Polyphonic aftertouch** (`pad.aftertouch`): while a pad is held, its reading from `release` to `full` is sent as poly pressure 0..127, when it moves by 2 or more, at most every 10 ms per pad, and 0 before the Note Off.
- **At start-up** a pad must read below `pad.release` once before it can play, so a pad held at power-up, or one with the wrong polarity (`inverted:`), stays silent. Such pads are named in a WARN log at the boot log (about 1.5 s).
- **EVENTs** (`debug.events`): type 7, the pad index, and on a hit the peak reading × 4 (0..16380), 0 on release. That is the raw number to set `threshold` and `full` from.
- **Presets** may set `pads: { <id>: { note: N } }` and `pad_settings: { channel, curve, aftertouch }`; the sensing (`threshold`, `release`, `full`, `window`, `retrigger`) is the pads' calibration and the same in every preset.
- **Generated code** (`gm_device.h` `gm_pad_t`, only with `pads:`): `GM_N_PADS` and the `GM_PAD_*_DEFAULT`s in `gm_device_config.h`, `gm_pads[GM_N_PADS]` (pin, name) and `gm_pad_note_defaults[]` in `gm_device_config.c`. A device without pads has no pad params, no "Pads" group and no pad code. RAM: about 16 bytes and one param value per pad, plus 8 values.

## Device YAML

Format 2 (`gemifi: 2`). It follows ESPHome's conventions where ESPHome has one: the pin schema, ids, `name`/`friendly_name`, hub components and time periods.

```yaml
gemifi: 2                      # format version
name: keystation-mini32        # node name: lowercase letters, digits and -, at most 31; names the built images
friendly_name: Keystation Mini 32   # display name: USB product string, HELLO, web UI (default: name)
project:
  version: 0.1.3               # firmware version (major.minor.patch): HELLO, .gfw header; default 0.1.0
board_id: 0x0001               # protocol/sysex.md section 7; 0x3F00-0x3FFF = DIY/private
platform: nuvoton-nuc1xx       # nuvoton-nuc1xx | stm32f1 | esp32 | esp32s2 | esp32s3 | esp8266
chip: NUC120LC1BN              # NUC: part; ESP: PlatformIO board id (e.g. esp32-s3-devkitc-1)
usb: { vid: 0x1209, pid: 0x0002 }   # native-USB platforms
midi: { transport: usb }       # ESP only: usb (S2/S3) | serial | ble (ESP32)
i2c: { sda: GPIO8, scl: GPIO9 }     # only if an I2C hub is used
scan_interval: 125us           # matrix/encoder scan period
boot_combo: [pitch_down, pitch_up]  # button ids

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

key_matrix:
  type: velocity
  rows: [PA8, PA9, PC0, PC1, PC2, PC3, PC6, PC7]
  columns: [PB0, PB1, PB2, PB3, PB4, PB5, PB8, PB12]
  keys: 32
  first_note: 48
  settle: 1us
  velocity: { fast: 1.6ms, slow: 120ms }
```

**Key matrix options.** `type: velocity` keys have two contacts. By default they are columns 2p and 2p+1 of one row (`contacts: columns`, the Keystation). `contacts: rows` puts them on rows 2p and 2p+1 of one column instead (the Code 61). `key_map` says which key sits where when the wiring doesn't follow the default numbering: one list per row (per row pair with `contacts: rows`), one key number per column (per column pair with `contacts: columns`), `null` where there is no key. Without it, key k is row k % rows, column (pair) k / rows. Rows can be MCU pins, sn74hc595 pins or decoder outputs; columns MCU pins or sn74hc165 pins.

```yaml
key_matrix:
  type: velocity
  contacts: rows
  rows: { decoder: rows, first: 0, count: 16 }
  columns: [ ... PB8-PB15 ... ]
  keys: 61
  first_note: 36
  key_map:
    - [55, 54, 53, 52, 51, 50, 49, 48]      # rows 0/1
    # ...
    - [null, null, null, 60, 59, 58, 57, 56]  # rows 14/15
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

**Format 1** files (`gemifi: 1`) are refused with one error per old key, at its line, saying what replaced it (`label:` → `id:`/`name:`, `pin: { pin: PA1 }` → `{ number: PA1 }`, `hubs:` + `type:` → per-chip lists, `pullup: true` → `mode: INPUT_PULLUP`, `shared: true` → `allow_other_uses: true`, `scan_us: 125` → `scan_interval: 125us`, and so on).

## Presets

A preset is a named set of *behaviours*: what the controls send, not how they are wired (research/core/presets-design.md). The YAML defines them; the user switches them at runtime with the preset button actions or from the editor (`preset.active`). A device without `presets:` has one preset, "Default", and works as before.

```yaml
presets:
  slots: 8                 # optional, 1..64, at least the number listed; default: the number listed
  revision: 1              # optional, 0..16383: bump it when the YAML presets change (the updater asks then)
  list:
    - name: Default        # slot 1: the components' own values (above); only name: and program:
    - name: Drums          # <= 16 printable ASCII, unique; default "Preset N"
      program: 9           # optional 0..127: the Program Change sent when it is selected
      midi: { channel: 10, transpose: 0, octave: 0 }   # start values (the octave buttons still move them)
      velocity: { curve: hard, fixed: 100 }            # curve: normal | soft | hard | fixed
      bend: { glide: 50ms }                            # 0..2s
      ramp: { time: 300ms }                            # 0..5s
      buttons:                                         # by id, with the button's own keys
        sustain: { action: note, note: 36 }
        edit:    { note: 38 }                          # action left out: keeps the button's
      analog:   { volume: { cc: 11 } }
      encoders: { encoder_1: { cc: 21, mode: relative_offset } }   # absolute | relative_twos | relative_offset
      pads: { kick: { note: 35 } }                     # pads: only note:
      pad_settings: { channel: 10, curve: soft, aftertouch: true }
```

- **Slot 1** is the first list entry (or "Default" when the list is empty); its values are the rest of the file. **Other presets** list only what differs from slot 1, by component id; an override equal to slot 1's value is dropped with a warning. **Empty slots** (`slots` minus the listed ones) start as copies of slot 1, named "Preset N", for the user to fill in.
- **What a preset can set** is the mapping: `button.action`/`button.number` (`action`, `cc`, `note`, `preset`, `mcu`), `analog.cc`, `encoder.cc`/`encoder.mode`, `pad.note`, and the per-preset globals above (`midi.channel`, `midi.transpose`, `midi.octave`, `velocity.curve`, `velocity.fixed`, `bend.glide`, `ramp.time`, `pad.channel`, `pad.curve`, `pad.aftertouch`), plus `program` (`preset.program`). The hardware side stays global and is refused in a preset, with where to set it: pins, LEDs, names, `adc_min`/`adc_max`, `takeover`, `steps_per_detent`, the key-time calibration (`key_matrix.velocity`), the pad sensing (`pad_settings` `threshold`, `release`, `full`, `window`, `retrigger`), `midi.transport`.
- **Storage.** The builder checks that every preset's worst-case stored record (every per-preset value changed, and a name) fits the platform's store, errors with the largest `slots` that fits, and warns above 85% (tools/builder/README.md "Storage budget"). The store keeps records in `include/config_store.h`'s v2 format (2 words a value), except on the STM32F1, which packs them (v3: one group per param, a byte or a halfword a value, `platform/stm32f1/src/config_store.c`) to fit its one 1 KB page: there the Code 61 takes up to 3 slots.

**Generated code** (`gm_device.h` `gm_preset_def_t`): `GM_N_PRESETS` (slots; 1 without `presets:`) and `GM_PRESETS_REVISION` in `gm_device_config.h`, and in `gm_device_config.c` `gm_presets[GM_N_PRESETS]` (slot n = `gm_presets[n - 1]`) with one sorted `{ param id, index, value }` array per preset that has overrides:

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

## Modes

A mode is a layer that turns the keys into function keys: while it is active, a key press runs what the mode binds to that key and sends no note (research/core/presets-design.md §6). It is how a keyboard with few buttons switches presets, the octave or the channel. Modes are device UI, so they are the same in every preset: a preset can't take away the way back.

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
```

- **Entering and leaving.** The toggle button enters the mode and, pressed again, leaves it, also from one of its sub-modes; there is no timeout. Keys held when the mode starts get their Note Off then, and their release sends nothing. A sub-mode (`parent:`) goes back to its parent after one action, so Edit → E3 → D4 picks preset 1 and leaves you in edit mode for another action, until Edit is pressed again.
- **The toggle button** has no action of its own (leave `action:` out): it only toggles the mode, and presets can't rebind it. A `button.action` set for it from the host is stored but not used. Its presses still show as EVENTs.
- **While a mode is active,** unbound keys are silent, and the buttons keep their normal actions.
- **Key actions:** `preset_select` (`preset: N`), `preset_next`, `preset_prev` (these need 2 or more preset slots), `octave_down`, `octave_up`, `octave_reset`, `transpose_down`, `transpose_up`, `transpose_reset`, `channel` (`channel: 1..16`), `velocity_curve` (`curve: normal | soft | hard | fixed`, velocity matrices only), and `{ mode: <id> }` to go to a sub-mode.
- **Ranges.** `keys:` may also hold one range, `white_from: <key>` (white keys only) or `from: <key>` (every key), with `action: preset_select` or `channel` and an optional `count:` (default: every slot, or 16 channels). The keys are numbered 1, 2, ... in order. Single keys can sit beside a range in the same mapping.
- **What it sends.** A preset switch from a mode is the same as from a button: Note Offs, PRESET_CHANGED and PARAM_CHANGED `preset.active`, and the preset's Program Change. Octave/transpose keys send PARAM_CHANGED like the octave buttons. `channel` and `velocity_curve` send PARAM_CHANGED and make the preset dirty, like a host edit: COMMIT keeps the change, a preset switch drops it.
- **The builder checks** that ids are unique, that every mode has either `toggle:` or `parent:`, that parents exist and don't loop, that every sub-mode is reachable from a key, that keys are on the keyboard and bound once per mode, and the actions' numbers.

**Generated code** (`gm_device.h` `gm_mode_t`, only with `modes:`): `GM_N_MODES` in `gm_device_config.h` (0 without modes: the mode code is then left out), and `gm_modes[GM_N_MODES]`, with one key table per mode sorted by key, `{ key index, GM_MK_* op, arg }`. The runtime keeps 1 byte for the active mode and 6 for the preset blink.

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

LED `feedback:` takes exactly one of `note:` / `cc:` (a number 0..127, with an optional `channel: 1..16`), `clock: beat`, `transport: playing`, or `mcu: <MCU LED>` (below). Values map to the LED as in Mackie Control: 0 off, 1 blinking, anything else on (the first colour of a bicolour LED). DAW scripts (F3) can also reach a bicolour LED's second colour, and set an RGB LED's colour (FB_LED_RGB). An RGB LED (`pin3:`) shows white for any level from its button, plain feedback, Mackie Control or FB_LED; only FB_LED_RGB picks a colour, with each channel on or off (on if it is at least half the brightest channel: 127/60/0 is red and green).

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

`daw:` tells a script what to connect the component to; the names are in `hosts/README.md` (or `x_<name>` for your own). Without `daw:`, a component's `mcu:` name is its role. With scripts on, `ring:` and `ring_center:` work without `mcu:`. A script's LED states don't time out (`led_timeout` doesn't apply) and `owner: local` LEDs ignore them; the script hands them back when it stops.

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
#define GM_MCU 0                          /* 1: mcu: (then also GM_MCU_CABLE, GM_MCU_ID) */
#define GM_USB_DAW_PORT 0                 /* 1: second USB-MIDI cable (then also GM_DAW_PORT_NAME) */
/* ESP only: */
#define GM_MIDI_TRANSPORT GM_MIDI_USB     /* GM_MIDI_USB / _SERIAL / _BLE, values in gm_hal.h */
#define GM_I2C_SDA 8                      /* GPIO numbers, when i2c: is given; */
#define GM_I2C_SCL 9                      /* otherwise the board's default Wire pins */
```
Optional build flags on ESP: `GM_MIDI_BAUD` (serial transport, default 115200), `GM_FW_VERSION` (`major << 24 | minor << 16 | patch`; normally set by the YAML `project: { version }`).
`gm_device_config.c`: `const gm_device_t gm_device = {...}` with its tables, and the default arrays `gm_button_action_defaults[]`, `gm_button_number_defaults[]`, `gm_analog_cc_defaults[]`, `gm_encoder_cc_defaults[]` (each at least one element long, so a count of 0 still compiles). Only when the YAML uses them: `gm_led_fb[]` (`GM_LED_FB`), `gm_analog_pickup[]` (`GM_ANALOG_PICKUP`), `gm_analog_mcu[]`, `gm_encoder_mcu[]`, `gm_encoder_ring[]` (`GM_MCU`). Plain `make` (firmware and `core/test`) generates them from `devices/keystation-mini32-mk3.yaml` into the build dir.
