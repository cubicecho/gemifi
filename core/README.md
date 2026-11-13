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
| `include/app_proto.h`, `src/app_proto.c` | app-side request handling: HELLO, REBOOT, identity, config, FW_* (via `hal_fw_ops`), LOG/EVENT/PARAM_CHANGED. Where the bootloader takes uploads (nuvoton-nuc1xx, stm32f1: `-DHAL_APP_FW_UPLOAD=0` in their `mk/app.mk`), FW_* and its buffers are left out and every FW_* is answered BAD_STATE; the receive buffer then holds the longest other request (SET_PARAMS with 16 values, 86 bytes of payload) |
| `include/gm_device.h` | the hardware model: virtual pins, hubs, buttons, LEDs, analog inputs, encoders, key matrix, pads |
| `include/gm_hal.h` | what a platform provides |
| `include/gm_controller.h`, `src/controller.c` | the runtime: scanning, actions, velocity, LEDs, MIDI out, host feedback (F1) |
| `src/midi_in.c` | incoming MIDI: USB-MIDI packets or a byte stream, sorted by port into SysEx (the protocol), feedback and MCU |
| `include/gm_feedback.h` | the feedback switches (`GM_FEEDBACK`, `GM_MCU`, ...) and the calls between the files above |
| `include/gm_mcu.h`, `src/mcu.c` | Mackie Control on the DAW port; empty unless the YAML has `mcu:` |
| `src/vpin.c` | virtual pins: MCU pins via the HAL, expander hubs in software, a driver per hub type |
| `src/gm_hub.h` | the hub driver interface (`gm_hub_ops_t`) and the per-hub state, inside the core |
| `src/settings.c` | the param table, sized from the device's component counts |

Rules: C11, no heap, no floating point, no libc beyond `<stdint.h>` (the NUC120 has 4 KB of RAM and 32 KB of flash). Headers are C++-safe (`extern "C"`), for platforms written in C++.

What each component costs (CPU, latency to MIDI out, MIDI traffic, RAM) and the YAML knobs that trade them: [performance.md](performance.md).

## Virtual pins and hubs

Every pin in a device is a `gm_pin_t { hub, flags, num }`:
- `hub = 0`: a pin of the MCU. `num` is the platform's pin number: NUC1xx `port * 16 + bit` (`PA0` = 0, `PB12` = 28, `PC7` = 39); ESP32 the GPIO number.
- `hub = n ≥ 1`: pin `num` of hub `n - 1` (chained shift registers continue: pins 8–15 are on the second chip). Hubs are numbered in the order they appear in the YAML.
- `flags`: `GM_PIN_INVERT` (YAML `inverted: true`: active-low, pressed / lit = electrical 0), `GM_PIN_PULLUP`, `GM_PIN_PULLDOWN` (YAML `mode: INPUT_PULLUP` / `INPUT_PULLDOWN`; MCU pins, and pull-ups on MCP23017 pins), `GM_PIN_OPEN_DRAIN` (YAML `mode: OUTPUT_OPEN_DRAIN`; only a `tm1628` or `tm1637` `dio_pin`, platform mode `HAL_OUT_OD`).

Hubs are declared ESPHome style, one top-level list per chip, named like the ESPHome component:

| Chip key (YAML) | `gm_device.h` | Pins it uses | Gives |
|---|---|---|---|
| `sn74hc595` | `GM_HUB_HC595` | `data_pin` (SER), `clock_pin` (SRCLK), `latch_pin` (RCLK), optional `oe_pin` (/OE); `sr_count` chips (1–4) | 8 × sr_count outputs. LEDs, matrix rows |
| `sn74hc165` | `GM_HUB_HC165` | `data_pin` (QH), `clock_pin` (CLK), `load_pin` (SH/LD); `sr_count` chips (1–4) | 8 × sr_count inputs, numbered as ESPHome does: 0–7 on the chip whose QH goes to the MCU (0 = its A), 8–15 on the next. Buttons, matrix columns |
| `cd74hc4051` | `GM_HUB_MUX` | `pin_s0`–`pin_s2`, `common_pin`, optional `enable_pin` (/E), `delay` (settle time after switching channel, default 2us) | 8 input channels, analog (if `common_pin` is an ADC pin) or digital. Muxes may share select lines (`allow_other_uses`); give them the same `delay` |
| `cd74hc4067` | `GM_HUB_MUX` | `pin_s0`–`pin_s3`, `common_pin`, optional `enable_pin`, `delay` | 16 input channels, as above |
| `mcp23017` | `GM_HUB_MCP23017` | I2C (`address`, default 0x20; the `i2c:` pins). ESP only for now: stm32f1 and nuvoton-nuc1xx have no I2C driver, so the builder refuses it there | 16 GPIO (`0`–`15` or `GPA0`–`GPB7`) |
| `pcf8574` | `GM_HUB_PCF8574` | I2C (`address`). ESP only for now, as `mcp23017` | 8 GPIO (`0`–`7` or `P0`–`P7`; quasi-bidirectional: inputs are written high) |
| `decoder` | `GM_HUB_DECODER` | `address` (1–4 pins, A0 first), optional `groups` (1–4 mappings of enable pin to level, e.g. `{ PB6: 1, PB7: 0 }`) | One-of-N row selects (74HC138-style chips sharing address lines): output n = address n % 2^address-pins in group n / 2^address-pins. Only matrix rows (`key_matrix`, `switch_matrix`) |
| `switch_matrix` | `GM_HUB_SWMATRIX` | `rows` (MCU pins or decoder outputs), `columns` (MCU input pins), `settle` (wait after driving a row, default 1us) | Up to 32 switches, scanned from the scan interrupt: pin n = row n / columns, column n % columns, 1 = closed. Buttons, encoders |
| `maudio_pad_leds` (was `rgb_serial`, still taken) | `GM_HUB_MAUDIO_PADS` | `tx_pin` (stm32f1: PA2 only); `rx_pin` (optional, the board's answers; stm32f1: PA3 only); `gamma` (1.0–3.0, default 1); `white` (`[r, g, b]`, default 255s); `count` (1–32 LEDs, default 16); `dma` (default true, see "DMA channels") | 3 × count outputs: pin = LED × 3 + colour (0 red, 1 green, 2 blue). The M-Audio Code pad LED board; see "M-Audio pad LEDs" |
| `ws2812` | `GM_HUB_WS2812` | `pin` (an MCU pin; stm32f1: PA7 or PB15); `count` (1–256 LEDs); `order` (`GRB`, the default, `RGB`, `BRG`, ...; `GRBW`, `RGBW`, ... for RGBW LEDs); `gamma`, `white` (`[r, g, b]`, `[r, g, b, w]` on RGBW) | 3 × count outputs (4 × on RGBW): pin = LED × 3 + colour (0 red, 1 green, 2 blue, 3 white). WS2812B / SK6812 strips; see "WS2812 LED strips" |
| `xy_pad` | `GM_HUB_XY` | `x_plus_pin`, `x_minus_pin`, `y_plus_pin`, `y_minus_pin` (MCU pins); `touch_min`, `touch_max` (default 570, 3430) | 2 analog channels, `x` (0) and `y` (1), for `analog:` inputs. See "XY pads" |
| `tm1628` | `GM_HUB_TM1628` | `clk_pin`, `dio_pin` (may be `OUTPUT_OPEN_DRAIN`), `stb_pin`; `ram_bytes` (1–16, default 14), `intensity` (0–7, default 7), optional `grids` (4–7), `fade` (default false) | 8 × ram_bytes outputs: display RAM bits. LEDs (see "TM1628 LED drivers") |
| `max7219` | `GM_HUB_MAX7219` | `clk_pin`, `din_pin`, `cs_pin` (LOAD); `num_chips` (1–8, default 1), `intensity` (0–15, default 15), `scan_limit` (digits scanned, 1–8, default 8) | 64 × num_chips outputs: pin = chip × 64 + digit × 8 + segment (0–6 = A–G, 7 = DP). 7-segment digits and LEDs (see "MAX7219, TM1637 and HT16K33") |
| `tm1637` | `GM_HUB_TM1637` | `clk_pin`, `dio_pin` (may be `OUTPUT_OPEN_DRAIN`); `length` (grids, 1–6, default 6), `intensity` (0–7, default 7) | 8 × length outputs: pin = grid × 8 + segment (bit 0 = SEG1 = a). 7-segment digits (see "MAX7219, TM1637 and HT16K33") |
| `ht16k33` | `GM_HUB_HT16K33` | I2C (`address`, default 0x70; the `i2c:` pins), `intensity` (0–15, default 15). ESP only for now, as `mcp23017` | 128 outputs: pin = COM × 16 + ROW (display RAM bit, byte × 8 + bit). 14- and 7-segment backpacks, LED matrices (see "MAX7219, TM1637 and HT16K33") |
| `ssd1306` | `GM_HUB_SSD1306` | I2C (`address`, default 0x3C; 0x78 / 0x7A, the 8-bit forms printed on many boards, are taken as 0x3C / 0x3D with a warning; the `i2c:` pins); `model` (`ssd1306`, the default, or `sh1106`); `width` × `height` (128 × 64, the default; an SSD1306 also 128 × 32, 96 × 16, 64 × 48, 64 × 32); `contrast` (0–255, default 207); `rotate` (0 or 180). ESP only for now, as `mcp23017` | No pins: `text` displays draw on it (see "SSD1306 and SH1106 OLEDs") |

A pin on a hub names the hub by its chip key and its id: `{ sn74hc595: leds, number: 3 }`, `{ cd74hc4051: faders, number: 0 }`. The MCP23017's pin key is `mcp23xxx` (as in ESPHome): `{ mcp23xxx: io, number: GPB7 }`.

Inputs on HC165 are read once per scan into a cache, and outputs on HC595 are written once per scan, so a whole key matrix can live on shift registers. MCP23017 and PCF8574 are read and written from the main loop (every 1 ms), since I2C can't run in the scan interrupt.

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

### Hub drivers

Each hub type is a driver in `src/vpin.c` behind one interface, `gm_hub_ops_t` (`src/gm_hub.h`), listed in `hub_ops[]` by type. `gm_hub_t` is the hub's type and that type's own config, a union member generated by the builder (`.sr`, `.mux`, `.i2c`, `.dec`, `.sm`, `.tm`, `.rgb`, `.xy`: `include/gm_device.h`); each hub's state in `vpin.c` is a few common fields (`in`, `out`, `written`, `channel`, ...) and a union of each type's own. The ops, all optional:

| Op | Called by | Runs |
|---|---|---|
| `init` | `vpin_init` | start-up: control pins to their idle levels, state to power-up |
| `input`, `output` | `vpin_input`, `vpin_output` | start-up: a pin is read / driven (I2C: direction and pull-up bits) |
| `read`, `write` | `vpin_read`, `vpin_write` | anywhere. Without them: the cached `in` / `out` words |
| `analog` | `vpin_analog` | the main loop: 0..4095 on a channel (mux, XY pad) |
| `rgb` | `vpin_rgb` | a whole colour on an LED (maudio_pad_leds, ws2812); a hub with it is full-colour: its LEDs take full-colour looks ("Full-colour LEDs") |
| `refresh`, `flush` | `vpin_refresh` / `vpin_flush` in the scan tick (HC165 / HC595), or with `GM_HUBF_BUS` `vpin_refresh_i2c` / `vpin_flush_i2c` from the main loop (I2C) | inputs into `in`; `out` to the chip if it changed |
| `send` | `vpin_poll` every main-loop pass, and `vpin_send_all` | what changed on a bit-banged chip (TM1628, MAX7219, TM1637), at most a budget of bus bytes a pass (4, ~50 µs on the STM32); none on a busy pass (the LED update's) |
| `poll` | `vpin_poll` every main-loop pass | protocol timing (maudio_pad_leds: one LED per 5 ms, the keepalive, acks; ws2812: a frame when something changed) |

`flags` say what else the core needs to know: `GM_HUBF_IN_REG` (`in` holds the pins' levels, so a key scan can read a column with one load), `GM_HUBF_NO_READ` (outputs only), `GM_HUBF_BUS`, `GM_HUBF_SELECT` (decoder outputs: "off" does nothing), `GM_HUBF_MATRIX`. The controller asks `vpin_caps()` / `vpin_pin_caps(pin)` (`VPIN_SCAN_IO`, `VPIN_BUS_IO`, `VPIN_MAIN`, `VPIN_MATRIX`, `VPIN_RGB`) which loops to run and what a pin can do, instead of looking at hub types.

The scan's hot paths don't go through the table: `vpin_read` on a mux channel and on a cached input, `vpin_write` on a decoder row and on a cached output, and the switch-matrix scan (`vpin_scan_matrices`, the hardware scan's `vpin_scan_plan` / `vpin_scan_feed`) are written out in `vpin.c`, so a pin costs what it did before the table. Hub types with more than pins keep their own calls in `gm_vpin.h`: the TM1628's `fade:`, the mux's hardware analog scan, the XY pad's `vpin_analog_jump`.

The builder writes `GM_HUB_TYPES` (a bit per hub type the device has) into `gm_device_config.h`; `hub_ops[]` holds only those drivers, so only they are linked (the Keystation, with no hubs, links none).

### Adding a hub type

1. **`include/gm_device.h`**: a `GM_HUB_*` value (before `GM_HUB_N_TYPES`), its config struct, and a member for it in `gm_hub_t`'s union. A config bigger than the largest (the decoder's, 36 bytes) makes every hub bigger: point at a `const` table instead (as the switch matrix's rows and the LED hubs' `curve` do).
2. **`src/vpin.c`**: a section with its ops and a `gm_hub_ops_t`, and a `HUB(GM_HUB_..., ...)` line in `hub_ops[]`. State it keeps goes in a struct of its own in `gm_hub_state_t`'s union (`src/gm_hub.h`), reset by `vpin_init` before its `init`. Pins kept in the hub RAM (more than 32 of them, or bytes per pin) take `ram_alloc(n)` bytes there in `init`; count them into `GM_HUB_RAM` in the builder (`ram = True`).
3. **`tools/builder/gemifi_device.py`**: a `HubType` subclass (its YAML key, `enum` and `num`, its `keys`, the control pins' `lines`, `roles`, `validate`, `c_fields` for its union member, and `c_tables` for anything the entry points at), and an instance in `HUB_DEFS`. A hub with an `rgb` op sets `full_colour = True` (and `colour_pins` if an LED isn't 3 pins): its LEDs then take full-colour looks, `led_defaults` and step colours as they are. Old names of a type go in `aliases`. The YAML key, the pin key, the schema and the "which hubs can be an input / output / analog" checks follow from it. Then `gemifi_build.py schema -o site/src/schema/device.json`.
4. **Tests**: a core test YAML in `core/test` with the hub, and a builder test of its validation and C.

### TM1628 LED drivers

`tm1628` covers the TM1628 and its relatives that take the same commands (TM1638, TM1620, …: set `ram_bytes` to what the chip has or what the board uses). They're output-only here: the key-scan inputs aren't read.

- **Pin numbers** are display RAM bits: `number = byte * 8 + bit`, bit 0 = the LSB of the byte. `{ tm1628: leds_a, number: 37 }` is byte 4 (address 0xC4), bit 5. On a TM1628 in its usual wiring, byte 2g is SEG1–SEG8 of grid g+1 and byte 2g+1 is SEG9–SEG14, but the board decides what each bit lights.
- **Sharing**: several chips can share `clk_pin` and `dio_pin` (`allow_other_uses: true` on each), each with its own `stb_pin`. TM1628 lines can't be shared with other hubs' lines.
- **Writing**: `vpin_write` only changes a RAM copy (`GM_HUB_RAM` bytes in `vpin.c`) and marks the chip dirty if a bit changed. The main loop writes dirty chips, never the scan interrupt: at most 4 bytes on the bus per pass, and not on the pass that works the LED levels out (every 10 ms). A chip's first write, at start-up, is its whole RAM: the display-mode command (`grids - 4`, only if `grids:` is set), 0x40 (write, auto-increment), 0xC0 and all `ram_bytes` bytes, then 0x88 | `intensity` (display on). Those stay set in the chip, so after that only what changed goes out: 0x40, then each run of changed bytes as one command, 0xC0 | its first address and its bytes. A run goes on over one unchanged byte (sending it again costs a bus byte, as a new address command would) and ends at two. One LED is 3 bus bytes (0x40, C0|a, the byte), as a fixed-address write (0x44) would be; 12 changed bytes in a row (about what the Code 61's 6-character display takes) are 14, where fixed addresses would take 25. A run the 4-byte budget cuts short goes on at its next address (a new 0xC0 | address) on the next pass. Bits go LSB first, DIO set while CLK is low and taken on the rising edge; the bus idles with STB, CLK and DIO high. Each chip is written once at start-up (all off after the lamp test), so it's cleared even with no LEDs on it.
- **Timing**: CLK low 600 ns (DIO is set at its start, so that's also DIO's setup time: an open-drain DIO has to rise through its pull-up within it), CLK high 450 ns, so a bit is at least 1.05 µs (the TM1628 takes up to 1 MHz, PWCLK ≥ 400 ns, setup and hold ≥ 100 ns); STB low 1 µs before the first bit, high 1 µs after the last one, and high for at least 1 µs between commands. The phases are counted on `hal_cycles()` from just after each edge (`hal_ns_to_cycles` converts, once per write), and the pins are written with one set/reset store each (`hal_pin_bsrr`), so on the STM32 at 72 MHz a bit is about 1.3 µs and a byte about 11 µs: the 4-byte budget is ~50 µs a pass, one LED ~40 µs. A platform without them (`hal_pin_bsrr` or `hal_ns_to_cycles` returning 0) uses `hal_pin_write` and `hal_delay_us(1)` a phase, about 2 µs a bit or more. `TM_CLK_LOW_NS` and `TM_CLK_HIGH_NS` in `vpin.c` set the phases; raise the low one if a slow DIO pull-up garbles bits.
- **Brightness** is per chip (`intensity`, the chip's PWM). LEDs on it are on/off, like other LEDs.
- **`fade: true`** lets a chip's brightness follow the host: a DAW's colour (FB_LED_RGB) sets the chip's 8 steps from the brightest LED on it, up to `intensity`, and below half the dimmest step the chip goes dark (display off, 0x80). Host colours win: any other lit LED or look counts as full only while no LED on the chip shows a host colour, so an LED the host turned off (showing its idle look) doesn't hold the chip at full. Every LED on the chip dims together, so it suits a chip with only LEDs on it (the Code 61's button chip) and not one that also drives a display. The steps are uneven (pulse widths of 1, 2, 4, 10, 11, 12, 13 and 14 sixteenths), so a fade is coarse at the bottom and nearly flat at the top.

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

### MAX7219, TM1637 and HT16K33

Three more LED drivers for segment digits (and single LEDs), written from the main loop like the TM1628: the MAX7219 and TM1637 bit-banged on any platform, the HT16K33 on the I2C bus (ESP only for now). Their pins are kept in the hub RAM (`GM_HUB_RAM` counts them too), and `send` writes only what changed, within the shared 4-byte budget, never on the LED update's pass. All three are untested on hardware.

- **`max7219`** (also the MAX7221; ESPHome's keys): a chain of `num_chips` chips on one `din_pin` / `clk_pin` / `cs_pin`. Chip 0 is the one whose DIN is the MCU's. Pin = chip × 64 + digit × 8 + segment, with segment 0–6 = A–G and 7 = DP, and digit 0 = DIG0, so a `segment7` character is its digit's first pin (`number: 0`, `8`, `16`, ...) and every chip is also 64 single LEDs. The chips' own decode is off: the core sends segments. The first write sets the chain up (display test off, no decode, scan limit `scan_limit - 1`, intensity) and writes every digit before turning it on (shutdown off), so nothing random shows. After that, a digit that changed in any chip is one frame: that digit register of every chip, the last chip's word first, taken on CS rising. Words go MSB first, DIN set while CLK is low and taken on the rising edge, each phase ≥ 100 ns on `hal_cycles()` (`MX_PHASE_NS`; 1 µs a phase on a platform without a cycle counter, the ESP and NUC builds). A frame counts 2 a chip against the budget, and at least one goes out per pass. Several chains can share `clk_pin` and `din_pin` (`allow_other_uses: true` on each), each with its own `cs_pin`. A low `scan_limit` lights each digit longer: the datasheet's "Scan-Limit Register" says how to choose RSET for fewer than 4.
- **`tm1637`** (ESPHome's keys, `length` = grids): a 2-wire chip, one per `clk_pin` / `dio_pin` pair (there's no chip select, so they can't be shared). Pin = grid × 8 + segment, bit 0 = SEG1 (a on the usual 4- and 6-digit modules) to bit 7 = SEG8 (the point, or the colon on clock modules). Start (DIO falls while CLK is high), bytes LSB first, each followed by the chip's ACK clock, stop (DIO rises while CLK is high), each phase 3 µs (`TM37_HALF_US`; the chip's clock is slow and modules often have capacitors on both lines), about 60 µs a byte. The ACK isn't read: DIO is driven low through it, the level the chip pulls it to, so a push-pull DIO never fights the chip (`OUTPUT_OPEN_DRAIN` with a pull-up works too). The first write is 0x40 (write, auto-increment), every grid from 0xC0, then 0x88 | `intensity` (on); after that each pass sends the first run of changed grids, 0x40 then 0xC0 | its address and as many bytes as the budget has room for (a byte counts 5), at least one.
- **`ht16k33`** (the Adafruit 14-segment and 7-segment backpacks and similar boards): 16 bytes of display RAM, pin = COM × 16 + ROW, so on a 14-segment backpack character i is pins 16i–16i+14. Adafruit wires the lower diagonals the other way round from `gm_display.h` (its L is the lower-left one, N the lower-right), so give the display `segment_order: [a, b, c, d, e, f, g1, g2, h, i, j, m, l, k, dot]` [from Adafruit's font table, not tried on a board]. The first write is 0x21 (oscillator on), the whole RAM from address 0, 0xE0 | `intensity`, 0x81 (on, no blink); after that, the first run of changed bytes as one write from its address, as many as the budget has room for (an I2C byte at 400 kHz counts 2), at least one. The key scan isn't read.

```yaml
max7219:
  - { id: digits, clk_pin: PA5, din_pin: PA7, cs_pin: PA4, intensity: 8, scan_limit: 3 }
tm1637:
  - { id: clock, clk_pin: PB6, dio_pin: PB7, length: 4 }
displays:
  - { id: preset_display, type: segment7, shows: preset_number,
      chars: [{ max7219: digits, number: 0 }, { max7219: digits, number: 8 }, { max7219: digits, number: 16 }] }
  - { id: text, type: segment7, shows: last_message,
      chars: [{ tm1637: clock, number: 0 }, { tm1637: clock, number: 8 }, { tm1637: clock, number: 16 }, { tm1637: clock, number: 24 }] }
```

### SSD1306 and SH1106 OLEDs

The small I2C OLEDs (0.96" 128 × 64 and 0.91" 128 × 32 SSD1306 modules, 1.3" SH1106 ones) are `ssd1306` hubs, written from the main loop on the I2C bus (ESP only for now). The hub keeps a framebuffer in the hub RAM (width × height / 8 bytes: 1 KB for 128 × 64, and as much again for what the panel was sent), and `text` displays draw characters into it (see "Displays"). It has no pins.

- **The first write** sets the panel up with the display off (clock, multiplex = height − 1, offset 0, start line 0; on an SSD1306 the charge pump on and page addressing, on an SH1106 its DC-DC on; segment remap and COM scan direction, reversed with `rotate: 180`; COM pins, `contrast`, pre-charge, VCOMH), writes every page (blank), then turns the display on, so nothing random shows.
- **After that**, each pass sends one run of changed bytes: its page and column (three commands, each after a 0x80 control byte), then 0x40 and up to 16 data bytes, one write of at most 23 bytes (~0.55 ms at 400 kHz; the ESP Wire buffer is 128). A run is bigger than the shared 4-byte budget, so a pass sends one and nothing else on the bus, and not on the LED update's pass. A whole new line of 10 characters at scale 2 (240 bytes) takes 15 passes. Only changed bytes go out, so a CC value changing on a big display rewrites a few characters' columns.
- **The SH1106** has 132 columns of RAM and shows 128 from column 2, and it only has page addressing: give `model: sh1106`. If the text is upside down, `rotate: 180`. If nothing shows, check the address: the module's 0x78 (as printed) is 0x3C, its 0x7A is 0x3D.
- Untested on hardware. The writes aren't checked, as the other I2C hubs' aren't: a missing panel costs its bytes once per change.

```yaml
i2c: { sda: GPIO21, scl: GPIO22 }
ssd1306:
  - { id: oled, address: 0x3C }          # model: sh1106 for a 1.3" module
displays:
  - { id: oled_main, ssd1306: oled, line: 1, scale: 2, shows: last_message }   # 10 big characters
  - { id: oled_channel, ssd1306: oled, line: 6, shows: channel }              # 21 small ones
```

### Full-colour LEDs

Two hub types drive LEDs that can show any colour, a byte per colour: `ws2812` (addressable strips) and `maudio_pad_leds` (the M-Audio Code pad LED board). An RGB LED on one of them (`pin`, `pin2`, `pin3` = one LED's red, green and blue: numbers LED × 3, + 1, + 2; LED × 4 on an RGBW strip) shows:

- full-colour looks: `idle: blue 20%`, `active: "#ff8000"` ("LED looks"), from its own `idle:` / `active:` or `led_defaults:`;
- a cycle step's full colour (`colour: "#ff8000"`, "Cycles");
- the colour a DAW script picks (FB_LED_RGB, 0..127 per channel, sent as 0..255).

Anything that only switches pins on or off (a plain on, a named look like `blue`, a mode's named colour) sends 0 or 255 per colour. An LED whose pins aren't one LED's red, green and blue in a row (or an LED elsewhere) is on/off per pin: a full-colour look there is an error, and in `led_defaults:` or a step colour it shows as the nearest on/off look.

Two settings on the hub calibrate what goes on the wire, so colours look right:

- **`gamma:`** eyes see brightness on a curve: a quarter of the power looks about half as bright. Each colour goes through 255 × (v / 255) ^ gamma first, so `blue 50%` looks half as bright as `blue`. Default 1 (straight). About 2.2 suits most LEDs; anything above 0 stays above 0.
- **`white:`** what full red, green and blue (and white, on RGBW) send, to balance the LEDs: `[255, 200, 220]` takes some green and blue out where 255/255/255 looks green. Default all 255.

To find them: show a ramp (255, 64, 16, 4) and see whether 64 looks about half as bright (gamma near 2.2), then show a grid of whites and pick the most neutral. The Code 61's values (`gamma: 2.2`, `white: [255, 200, 220]`) came from that.

### WS2812 LED strips

`ws2812` drives a chain of addressable LEDs, WS2812B or SK6812 (RGB or RGBW), on one MCU pin. Each LED is 3 hub pins (4 on RGBW) as on `maudio_pad_leds`, so LEDs take `pin`, `pin2`, `pin3` and show any colour ("Full-colour LEDs"):

```yaml
ws2812:
  - { id: strip, pin: GPIO48, count: 16, order: GRB, gamma: 2.2, white: [255, 235, 210] }
led_defaults: { idle: blue 20%, active: white }       # full colours where an LED can show them
leds:
  - { id: led_1, pin: { ws2812: strip, number: 0 }, pin2: { ws2812: strip, number: 1 }, pin3: { ws2812: strip, number: 2 } }
  - { id: led_2, pin: { ws2812: strip, number: 3 }, pin2: { ws2812: strip, number: 4 }, pin3: { ws2812: strip, number: 5 }, idle: "#ff6000" }
```

- **`order:`** the colours in the order the LED takes them: `GRB` (WS2812B, SK6812 RGB; the default), `RGB`, `BRG`, ..., and `GRBW`, `RGBW`, ... for RGBW LEDs (SK6812 RGBW). With a W, each LED is 4 pins (red, green, blue, white) and `white:` takes 4 values. The white part of a colour (the least of red, green and blue) goes on the white LED, so `white` or `#ffffff` lights only the white LED; the white pin on its own (`{ ws2812: strip, number: 4 * LED + 3 }`) is a single-colour LED.
- **When a frame goes out:** pins only change a copy in RAM; the main loop builds a frame (order, `gamma:`, `white:` applied) only when a colour changed, and sends it only if it differs from the last, at most every 5 ms (`WS_FRAME_MS`), never while the last frame and its latch (280 us low) are still going out, and never from the scan interrupt. The platform sends it in the background (`hal_ledstrip_*`), so the main loop doesn't wait either: a frame is 30 us an LED on the wire (16 LEDs about 0.5 ms).
- **ESP32, S2, S3:** RMT, ESP-IDF's TX driver with a bytes encoder (`platform/esp/gemifi_esp/ledstrip_esp.c`): any output pin, a TX channel a strip (ESP32 8, S2 and S3 4).
- **stm32f1:** the SPI's MOSI with DMA: PA7 (SPI1, DMA1 channel 3) or PB15 (SPI2, channel 5), one strip each. The SPI runs at about 2.25 MHz and each LED bit is three SPI bits (0 = 100, 1 = 110: 0.44 / 0.89 us high, 1.33 us a bit). The DMA channel goes through the allocator ("DMA channels"). RAM: the hub's bytes twice (the colours and the last frame), and 3 bytes per colour byte for the SPI frame, plus 91.
- **Not supported:** the NUC (no driver yet); the builder refuses it. APA102 (clock and data) isn't there yet.
- **Wiring:** WS2812B take 5 V data; at 3.3 V most work, a 74AHCT125 level shifter makes sure. A resistor (about 330 ohm) in the data line and a big capacitor across the strip's supply are good practice; at full white a WS2812B draws about 60 mA.

### M-Audio pad LEDs

`maudio_pad_leds` (called `rgb_serial` before; that name still works, as the list key and the pin key, and builds the same firmware) is the M-Audio Code pad LED board: a small controller that drives the pads' RGB LEDs and takes their colours over a serial line, in its own protocol (Code 61: USART2 TX on PA2, 115200 baud 8N1), found in the stock firmware: one message per LED, `02, 0x80 + LED, R, G, B, 03` (each colour 00 off to FF full), and a lone `A5` keepalive at least every ~60 ms, without which the board turns its LEDs off. The board drops messages sent back to back, so the core sends one changed LED per 5 ms from the main loop (16 LEDs all changing take 80 ms) and the keepalive every 50 ms, with every write, the keepalive included, at least 5 ms after the last. The UART runs in the background (DMA on stm32f1, `hal_serial_write`).

At power-on the board isn't listening for the first ~220 ms, so the first colours are lost. Without `rx_pin`, every LED is sent once more 2 s after start; after that an LED goes out only when it changes. Don't resend LEDs on a timer: resending one every 50 ms left the Code 61's board deaf after a cold start.

With `rx_pin`, the core listens to the board, which answers each LED message with `0x40 + LED` (the keepalive gets no answer). An LED counts as shown only once answered. Without an answer in 20 ms it goes out again, after a pause that grows with each miss in a row (50 ms, 100 ms, ... up to 500 ms), so a board that is still starting gets little traffic. The first answer means the board is up, and every LED is sent again then instead of at 2 s. If nothing answers in the first 3 s, the hub works as without `rx_pin`.

```yaml
maudio_pad_leds:
  - { id: pad_leds, tx_pin: PA2, rx_pin: PA3, count: 16, gamma: 2.2, white: [255, 200, 220] }   # dma: false sends from the interrupt ("DMA channels")
leds:
  - { id: pad_1_led, pin: { maudio_pad_leds: pad_leds, number: 36 }, pin2: { maudio_pad_leds: pad_leds, number: 37 }, pin3: { maudio_pad_leds: pad_leds, number: 38 } }
pads:
  - { id: pad_1, pin: { cd74hc4051: pad_mux_b, number: 4 }, note: 36, led: pad_1_led }   # lit while held
```

Each colour is a byte (0 off to 255 full), and an RGB LED on the hub shows any colour, with the hub's `gamma:` and `white:` ("Full-colour LEDs").

### XY pads

A 4-wire resistive XY pad is two plates that touch where pressed. To read X, the core drives `x_plus_pin` high and `x_minus_pin` low, and reads the voltage of the floating Y plate. That reading is the touch position. Y is read the other way round. Each axis is read on the other plate's minus pin if that pin has an ADC, otherwise on its plus pin. The other plate is always released before a plate is driven, so a touch never joins two driven pins.

The core reads one axis per millisecond, in turn, in software. Each plate gets a full millisecond to settle, and each axis is new every 2 ms. On stm32f1 the reads use ADC2, so the pad works alongside `hardware_scan.analog`. That scan leaves the pad out of its plan.

A touch counts once both readings have been inside `touch_min`..`touch_max` four times in a row (each axis twice, about 4 ms). The first reading as a finger lands can be anywhere. Untouched, the floating plate reads outside that window. While untouched, the inputs send nothing and hold their last value. A new touch jumps straight to its position, without gliding there through the smoothing filter.

```yaml
xy_pad:
  - { id: xy, x_plus_pin: PB1, x_minus_pin: PC5, y_plus_pin: PC4, y_minus_pin: PB2 }
analog:
  - { id: xy_x, pin: { xy_pad: xy, number: x }, cc: 16, adc_min: 592, adc_max: 3408 }
  - { id: xy_y, pin: { xy_pad: xy, number: y }, cc: 17, adc_min: 592, adc_max: 3408 }
```

The axes are ordinary analog inputs: CC or channel pressure, per-preset settings, calibration with `adc_min`/`adc_max`. They can't be `pitch_bend`, because bend centres on the power-up reading and the pad reads nothing until it's touched.

### LED walk (finding LEDs)

To find which output drives which LED on new hardware, list every candidate output under `leds:` and build with `GM_LED_WALK_MS` set (stm32f1: `make LED_WALK=100`). Each LED then lights on its own for that many milliseconds, in `leds:` order, and the device log shows `walk LED <n>`. Holding any button pauses the walk. Normal LED behaviour is off in such a build; without the define the code isn't built at all.

## Components

Every component has an `id` (what the rest of the file uses to refer to it) and/or a `name` (display text). See "Ids and names" below.

| Component | Fields | Params it creates (per item, as arrays) |
|---|---|---|
| button | `id`/`name`, `pin`, `led` (an LED id, optional), default `action` and its `cc:` (CC actions), `note:` (action `note`), `mcu:` (action `mcu`), `program:`, `mmc:` or `sysex:`; optional `bank:`, `channel:`, `min:`, `max:` ("MIDI messages") | `button.action`, `button.number` |
| LED | `id`/`name`, `pin`, optional `pin2` (bicolour: the second colour), optional `pin3` (with `pin2`: an RGB LED, `pin` red, `pin2` green, `pin3` blue), optional `idle:` / `active:` ("LED looks" below), optional `feedback:`, `owner:`, `timeout:` ("DAW feedback") | – (LEDs show their button's state, or the octave/transpose level, or what the host sends) |
| analog (pot, slider, fader, expression, wheel) | `id`/`name`, `pin` (ADC pin or mux channel), `adc_min`, `adc_max` (the raw 0..4095 reading at the bottom and top; swap them to reverse), default `cc`, default `mode` (`cc`; `pitch_bend` or `channel_pressure`: no `cc` needed; "Pitch wheels and aftertouch" below; `cc14`, `nrpn`, `nrpn14`, `rpn`, `rpn14` with `nrpn:` / `rpn:`, `sysex` with `sysex:`: "MIDI messages"), optional `channel:`, `min:`, `max:`, `takeover:`, `touch:` ("DAW scripts"), `mcu:` | `analog.cc`, `analog.mode` |
| pad (velocity pad: pressure / FSR) | `id`/`name`, `pin` (ADC pin or mux channel; `inverted: true` if pressure lowers the reading), default `note`, optional `led` (lit while the pad is held); what all pads share goes in the top-level `pad_settings:` ("Pads" below) | `pad.note`, and once per device `pad.channel`, `pad.curve`, `pad.aftertouch`, `pad.threshold`, `pad.release`, `pad.full`, `pad.window`, `pad.retrigger` |
| encoder | `id`/`name`, `pin_a`, `pin_b`, `steps_per_detent`, default `cc`, default `mode` (below; `nrpn:` / `rpn:` for those modes), optional `channel:`, `min:`, `max:`, `mcu:`, `ring:`, `acceleration:` (the default in the top-level `encoder_settings:`; "Encoder acceleration" below) | `encoder.cc`, `encoder.mode` |
| key matrix (at most one) | `rows`, `columns`, `type` (`switch` / `velocity`), `keys`, `first_note`, `settle` (time after driving a row, default 1us), and for `type: velocity` optional default key times `velocity: { fast, slow }` (time between the two contacts for velocity 127 and 1; default 1.2ms and 120ms) | the velocity params if `type: velocity` |

Global params (always present): `midi.channel`, `midi.transpose`, `midi.octave`, `bend.glide`, `ramp.time`, `debug.events`, `debug.log_level`, and the read-only `config.hash` ("Config hash" below). The list with ids is in `src/settings.c`; ids never change meaning.

**Key matrix.** One row is driven at a time (to the active level: high, or low with `inverted`), the columns are read, then the row is released. `type: switch`: key = row + rows × column. `type: velocity`: two contacts per key; key k is on row k % rows and columns 2p, 2p+1 with p = k / rows. Whichever contact closes first starts the timer, the second sends Note On with a velocity from the time between them, and the key is released when both are open. Put diodes on the matrix if more than one key can be held.

**Button actions** (`button.action` values; YAML names in brackets): 0 none (`none`), 1 CC momentary (`cc_momentary`), 2 CC toggle (`cc_toggle`), 3 CC ramp (`cc_ramp`: glides to 127 while held, back to 0 on release), 4 note (`note`), 5 octave down (`octave_down`), 6 octave up (`octave_up`), 7 transpose down (`transpose_down`), 8 transpose up (`transpose_up`), 9 pitch bend down (`bend_down`), 10 pitch bend up (`bend_up`), 11 program next (`program_next`), 12 program previous (`program_prev`), and on devices with 2 or more preset slots (see "Presets") 13 next preset (`preset_next`), 14 previous preset (`preset_prev`), 15 select preset (`preset_select`: `button.number` is the slot, 1..slots, from the YAML's `preset:`, e.g. `{ action: preset_select, preset: 3 }`), and on devices with `mcu:` only, 16 Mackie Control (`mcu`; it was 13 before presets, so a profile stored with it needs setting again). `button.number` is the CC or note number: the YAML's `cc:` for the CC actions, `note:` for `note`, the slot for `preset_select`, the MCU note for `mcu` (from `mcu: play` etc.). The MIDI message actions, only on devices that use them ("MIDI messages"): 17 program (`program`, `program:` 0..127), 18 panic (`panic`), 19 MMC (`mmc`, `mmc: play` etc.), 20 SysEx (`sysex`, `sysex:` a string's id). On devices with pads: 21 pad octave down (`pad_octave_down`), 22 pad octave up (`pad_octave_up`): `pad.octave` ∓ 1, both held: back to 0, as the octave buttons. Other actions take neither (a `cc:`/`note:` there only sets the default number for when the action is changed in the config, with a warning).

**Encoder modes** (`encoder.mode`, YAML `mode:`): 0 absolute (`absolute`: 0..127, sends the value), 1 relative two's complement (`relative_twos`: 1 = +1, 127 = −1), 2 relative offset (`relative_offset`: 65 = +1, 63 = −1), 3 relative sign-magnitude (`relative_sign`: 1 = +1, 65 = −1), and with the message kinds ("MIDI messages") the absolute 4 `cc14`, 5 `nrpn`, 6 `nrpn14`, 7 `rpn`, 8 `rpn14`.

### Encoder acceleration

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

### LED looks (idle and active)

By default an LED is dark while off and lit (white on an RGB LED) while on. `idle:` and `active:` change that: `idle:` is what it shows while off, so a panel can be lit all the time and still show presses, and `active:` what it shows for "on" (held, toggled on, a pad hit, a host's on).

```yaml
led_defaults: { idle: blue, active: white }   # optional: every LED, unless it sets its own (full colours too: blue 20%)
leds:
  - { id: rec_led, pin: PC4, idle: on }                                # lit until pressed (active: off)
  - { id: bi_led, pin: PC5, pin2: PC6, idle: colour2, active: colour1 }
  - { id: b1_led, pin: PB0, pin2: PB1, pin3: PB2, idle: blue, active: white }
```

| LED | Looks | Defaults (idle / active) |
|---|---|---|
| single colour (`pin`) | `off`, `on` | off / on |
| bicolour (`pin2`) | `off`, `colour1` (`pin`), `colour2` (`pin2`), `both`; `on` = colour1 | off / colour1 |
| RGB (`pin3`) | `off`, `red`, `green`, `blue`, `yellow`, `cyan`, `magenta`, `white`; `on` = white | off / white |
| RGB on a full-colour hub (`ws2812`, `maudio_pad_leds`) | any colour too: `"#rrggbb"` (quoted: `#` starts a YAML comment), or a colour name with a brightness, `blue 20%` | off / white |

- `on` and `off` work with or without quotes.
- **`active:` left out** with an `idle:` that matches the default active look: active is `off`. So `idle: on` on a single-colour LED means lit until pressed, then dark. Giving both the same look is a warning: the LED would never show its state.
- **`led_defaults:`** takes any look of any kind of LED, full colours included, and each LED shows the nearest it can: a full colour as it is on an LED that can show any colour, elsewhere the channels at least half the brightest (`"#ff8000"` is red + green, yellow; black is `off`); a named look the same pins where it has them (yellow = red + green = `both` on a bicolour LED), else its first colour. A single-colour LED is `on` for anything but `off`, and then dark when active if the two come out the same. An LED's own `idle:` / `active:` win over the defaults.
- **What follows the looks:** off (0) shows idle and a plain on shows active, from every source: buttons, pads, modes, plain host feedback and Mackie Control. A blink alternates active and idle. A bicolour LED's other levels keep their meaning (octave +2 both colours, +3 colour 2, +4 colour 2 blinking against idle), and so does a colour a DAW script picks (FB_LED_RGB). The lamp test at start and the LED walk light every pin whatever the looks.
- The TM1628 and the MCU pins are on or off, so idle can't be a dim version of active there: pick a different colour, or `on` / `off`. Only an LED of a full-colour hub (`ws2812`, `maudio_pad_leds`; "Full-colour LEDs") takes full colours in its own `idle:` / `active:` (`idle: blue 20%`, `active: "#ff8000"`); elsewhere they are an error. Brightness in a full colour is as seen: the hub's `gamma:` turns it into power.
- **Generated:** `gm_led_t.looks` = `GM_LED_LOOKS(idle, active)`, each a pin mask (bit 0 `pin`, 1 `pin2`, 2 `pin3`), or 0 for the defaults; `gm_led_t.rgb` = the full colours, `GM_LED_RGB(0xRRGGBB)` each, or 0. A full-colour look's mask is its nearest on/off colour.

### Pitch wheels and aftertouch

An analog input with `analog.mode` = pitch bend (YAML `mode: pitch_bend`, or set from the web app) sends 14-bit pitch bend on `midi.channel` instead of a CC; with `analog.mode` = channel pressure (`mode: channel_pressure`) it sends channel pressure (aftertouch) 0..127, `adc_min` = released, `adc_max` = pressed hard, with the CC's hysteresis.

```yaml
analog:
  - { id: pitch_wheel, name: Pitch, pin: PA4, mode: pitch_bend }
  - { id: mod_wheel, name: Mod, pin: PA5, cc: 1 }     # a mod wheel is a plain CC 1
```

- **Centre.** The input's first reading after power-up is the centre, because a sprung wheel rests there; keep it untouched while the device starts. 16 raw ADC counts either side of it send no bend, so the wheel always returns to exactly 8192.
- **Range.** From the centre to each end (`adc_min` / `adc_max`, less 8 counts) is one half of 0..16383, so the bend is symmetric even when the centre isn't in the middle of the travel.
- **Noise.** Changes under 6 raw counts are dropped, except to the centre or an end. Both are raw counts, so a wheel that only moves the reading over part of 0..4095 (set `adc_min` / `adc_max` to its ends) is no twitchier.
- **Rate.** Pitch-bend and pressure inputs are read every 2 ms; the other analog inputs take turns, one every 2 ms.

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
- **A hit.** Readings are 0..4095 (after `inverted`). When a pad reaches `pad.threshold`, its peak over the next `pad.window` ms is taken (4 readings at 3 ms) and Note On goes out with velocity 1..127, linear from `threshold` to `full`, then shaped by `pad.curve` like the keys' curves (soft, hard; fixed = `velocity.fixed`). A pad let go within the window plays at once with the peak so far. It is released (Note Off) below `pad.release`, and can't start a new hit for `pad.retrigger` ms after that (bounce, double hits). The thresholds are live params: tune them from the web app.
- **Pad octave** (`pad.octave`, -4..4, per preset, starts at 0): every pad's note + 12 × it; a note that would go past 0..127 plays in the nearest octave in range. The `pad_octave_down` / `pad_octave_up` button actions move it, and the display shows `PAD +1`. A hit held across a change gets the Note Off of the note it played.
- **Polyphonic aftertouch** (`pad.aftertouch`): while a pad is held, its reading from `release` to `full` is sent as poly pressure 0..127, when it moves by 2 or more, at most every 10 ms per pad, and 0 before the Note Off.
- **At start-up** a pad must read below `pad.release` once before it can play, so a pad held at power-up, or one with the wrong polarity (`inverted:`), stays silent. Such pads are named in a WARN log at the boot log (about 1.5 s).
- **EVENTs** (`debug.events`): a pad's own hits only play notes. A pad's layer twin ("Button layers") sends EVENT type 7 instead: its pad component index (the pads, then the twins), and on a hit the peak reading × 4 (1..16380), 0 on release. That is also the raw number to set `threshold` and `full` from: hit the pads with their layer on.
- **Presets** may set `pads: { <id>: { note: N } }` and `pad_settings: { channel, curve, aftertouch }`; the sensing (`threshold`, `release`, `full`, `window`, `retrigger`) is the pads' calibration and the same in every preset.
- **Generated code** (`gm_device.h` `gm_pad_t`, only with `pads:`): `GM_N_PADS` and the `GM_PAD_*_DEFAULT`s in `gm_device_config.h`, `gm_pads[GM_N_PADS]` (pin, name, LED) and `gm_pad_note_defaults[]` in `gm_device_config.c`; with pad twins also `GM_PAD_TWINS` and `gm_pad_twins[]` (`gm_pad_twin_t`: layer, base pad, LED). A device without pads has no pad params, no "Pads" group and no pad code. RAM: about 16 bytes and one param value per pad, plus 8 values.

## Software scan

Without `hardware_scan:`, the key matrix is scanned from the scan tick (`scan_interval`): each row in turn is driven, the columns are read after `settle`, and the row is released. Rows on MCU pins are driven by one store each (`hal_pin_out_reg`), and columns on MCU pins are read as runs of adjacent bits of their port (`hal_pin_in_reg`; the Keystation's PB0–PB5, PB8, PB12 are 3 runs from one load), not pin by pin.

When every row is such a pin, every column is on one input register, nothing is on a shift-register hub and a key's contacts are on columns (the Keystation), `gm_init` picks a loop of its own (`matrix_scan_fast` in `controller.c`). The pins see the same thing: row on, `settle`, read, row off. A row that reads as it did on the last tick (nearly always) is then done; only a row that changed has its columns sorted out of the port bits and its keys looked at. On the NUC120 that is about 38 cycles per row besides the settle, down from about 200, so the Keystation's 8 rows take about 21 us of each 125 us tick instead of about 48 (the `scan load` the firmware logs 2.5 s after boot). A row is released sooner after its read than before, and the next one driven sooner, so a column has about 1 us less to fall back between rows: if a board shows ghost notes on the next row, raise `settle`.

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
- **Timing.** A key's two contacts are timed in frames (`row_time` × rows: 84 us on the Code 61 at 3.5 us), instead of the `scan_interval` tick, and velocity is worked out in microseconds, so finer timing gives finer velocities at the fast end. The scan tick keeps running for whatever else it does (shift-register hubs, encoders on MCU pins); matrices and encoders on switch matrices move to the frames. The panel's `rows_per_scan` doesn't apply: every row is in every frame.
- **Precharge.** A column that a closed key pulled low has only the MCU's weak pull-up (30–50 kΩ) to come back up, which is why `settle` has to be microseconds. With `precharge: true` the columns are driven to their idle level (the level their pull-up or pull-down gives) for a moment before each row, so the read only waits for a closed contact to pull its column down. On the Code 61 the keys are fine at `row_time: 1.5us, settle: 1us` (36 us frames), but a lightly pressed panel button (a carbon contact of a few kΩ) can need longer, and then reads as let go mid-press: its YAML uses `3.5us` / `3us` (84 us frames). While a key on the row being left or entered is held, its column is briefly driven against the row (about 0.25 us per row), so it needs a matrix with diodes or tolerant drivers. It's on in the Code 61 YAML (tried on hardware: no ghost or missing notes); leave it off on other boards until tried. With precharge, every column must be in one of the port's two config registers, set up as a pulled input, and nothing else may reconfigure those pins while the scan runs.
- **Buttons** on a switch matrix are read in every frame (in software, on every scan of their row). The 1 ms button sample sees whether a button read closed in every scan since the sample before, in some, or in none: a press counts on a sample that read it closed throughout, and a release once no scan has read it closed for 16 ms. So a contact that flickers during a light press stays one press, and one noisy reading doesn't press.
- **Checks.** The builder wants `row_time` ≥ `settle` + 0.25 us (+ 0.5 us with precharge), and `settle` ≥ 0.5 us with precharge. At boot the stm32f1 logs the frame length, the longest frame-processing interrupt and how often it fell behind (a flash write stalls it; frames lost then still count as time).
- **Analog** (`analog: true`): the analog inputs and pads are read in sweeps by the hardware. Every mux used must share the same select lines (one set/reset register), and nothing may read a mux digitally (a button on a mux). Each step of a sweep stores the select lines, waits the muxes' `delay:`, and converts every mux's common pin plus every analog input or pad on an ADC pin. The Code 61's 8 steps × PA7, PA6, PA1, PA0 take 200 us. `vpin_analog()` then returns the latest reading (an ADC pin: the mean of its readings in the sweep), the analog inputs are all read every millisecond instead of one every 2 ms, and the pads see their strongest reading of the sweeps since their last pass (about 5), so a strike's peak isn't lost between passes. The main loop does no conversions or mux settles at all.
- **Code.** The core side is in `controller.c` (`gm_scan_plan()`, `gm_scan_frames()`, `gm_controller.h`) and `vpin.c` (`vpin_scan_plan()`, `vpin_scan_feed()`, `gm_adc_plan()`, `gm_adc_sweep()`, `vpin_analog_peak()`); the STM32F1 drivers are `platform/stm32f1/src/scan_dma.c` and `adc_dma.c`. Host tests: `core/test` `test_matrix_hw` (`matrix-hw-test.yaml`) and `test_pads_hw` (`pads-hw-test.yaml`).

### DMA channels (stm32f1)

The STM32F103 has 7 DMA channels, and each peripheral's DMA request is wired to one of them (RM0008 table 78). The YAML decides which features get DMA, and the builder hands out the channels:

| Feature | DMA requests | Without DMA |
|---|---|---|
| `hardware_scan.matrix` | 2 of one timer (select, read); with `precharge` 4 (+ columns in, precharge) | the scan interrupt reads the matrix |
| `hardware_scan.analog` | ADC1 (channel 1) + 1 of one timer (mux step) | the main loop reads one input per pass |
| `maudio_pad_leds` (`dma: true`, the default) | USART2_TX (7) | `dma: false`: the USART2 interrupt sends the bytes |
| `din_midi` (`dma: false`, the default) | `dma: true`: USART1_TX (4), USART1_RX (5) | the USART1 interrupt moves the bytes |
| `ws2812` on PA7 / PB15 | SPI1_TX (3) / SPI2_TX (5) | none: a strip always sends by DMA (move it to the other pin) |

The UARTs' and SPIs' channels are fixed (two of them on one channel, as a PB15 strip and `din_midi dma: true`, is an error). The scans' depend on the timer that paces them, since each timer's update and compare events are wired to their own channels:

| Timer | UP | CH1 | CH2 | CH3 | CH4 | Matrix | Analog (ADC1 trigger) |
|---|---|---|---|---|---|---|---|
| TIM1 | 5 | 2 | 3 | 6 | 4 | yes (5 channels to choose from) | no: its triggers drive CC outputs, and PA9/PA10 are USART1's |
| TIM2 | 2 | 5 | 7 | 1 | 7 | yes (CH2 and CH4 share 7; precharge needs channel 1, ADC1's) | CC2 (mux on CH1 or CH4) |
| TIM3 | 3 | 6 | – | 2 | 3 | no precharge only (3 channels) | TRGO = OC2REF (mux on CH1, CH3 or CH4) |
| TIM4 | 7 | 1 | 4 | 5 | – | yes (precharge needs channel 1, ADC1's) | CC4 (mux on CH2 or CH3) |

`timer: auto` (the default) tries the matrix on TIM1, TIM3, TIM4, TIM2 and the analog scan on TIM4, TIM3, TIM2, taking the first that leaves the UARTs' channels free. For the Code 61 that is the original map: matrix on TIM1 (select = update, ch5; columns in = CC1, ch2; read = CC2, ch3; precharge = CC3, ch6), analog on TIM4 (mux = CC2, ch4; ADC1 by CC4). Only the select may be the update event; when it is a compare instead, every step of the row comes one timer tick (14 ns) later, which changes nothing else. `timer: tim3` (etc.) fixes the timer, and auto still picks its compares. TIM2 is also the core's scan tick (an interrupt, no DMA): when a scan takes it, the tick moves to TIM3 or TIM4.

When nothing fits, the build stops. The message names both users and says what would fit: `din_midi.dma: needs DMA1 channel 4 (USART1_TX), which hardware_scan.analog uses (TIM4_CH2): set din_midi dma: false (...), or turn hardware_scan.analog off; or turn hardware_scan.matrix precharge off: the scan then needs 2 channels instead of 4, and that fits`. An explicit timer that can't run its scan is an error on its `timer:` line, with the timer's requests. The build output and the top of `gm_device_config.h` show the map and the timers, for example `DMA1 channels: 1 ADC1 (hardware_scan.analog), 2 TIM1_CH1 (hardware_scan.matrix columns in), ... 7 USART2_TX (maudio_pad_leds 'pad_leds'); free: none` and `timers: TIM1 hardware_scan.matrix, TIM2 scan tick, TIM4 hardware_scan.analog (ADC1 started by TIM4_CC4)`.

What fits on the Code 61 (ADC1, the mux step and the matrix leave 7 − 2 − 4 = 1 channel with precharge, 3 without):

| Matrix | Fits with it |
|---|---|
| precharge (36 us frames) | `maudio_pad_leds` DMA (the default), DIN by interrupt |
| no precharge (`row_time: 3.5us, settle: 3us`: 84 us frames) | `maudio_pad_leds` DMA and `din_midi dma: true`: matrix on TIM1 (select CC1 = ch2, read CC2 = ch3), analog on TIM3 (mux CC1 = ch6, ADC1 by TRGO), DIN on 4 and 5 |

The UARTs gain little from DMA: at 31250 baud a DIN MIDI byte takes 320 us, and the interrupt for it takes about a microsecond. Spend the channels on the scans, and use DMA for the UARTs on boards that don't scan by hardware.

## Displays

A display is a row of segment characters on output pins of hubs (a TM1628, MAX7219, TM1637, 74HC595 ...), or a row of text on an OLED. Up to 4 displays, of up to 16 characters each (a text display up to 21), one row each.

- **Types.** `segment14` (the default; the Code 61's): 15 pins a character, segments a b c d e f g1 g2 h i j k l m, then the dot. `segment7`: 8 pins a character, segments a b c d e f g, then the point. Both are drawn in `core/include/gm_display.h`. `text`: 5 × 7 characters on an `ssd1306` hub (below). Letters show as capitals (on 7 segments as best they can: b, d, n, r and t in lower case, K M W X approximated), a `.` lights the previous character's dot, and characters with no glyph are blank. The fonts are `gm_seg14_glyph()` and `gm_seg7_glyph()` in `core/src/display.c`.
- **Text displays** (`type: text`, the default when `ssd1306:` is given): `ssd1306:` the OLED's id, `line:` the text line its top is on (8 pixels a line from the top: 0–7 on a 64-pixel panel, 0–3 on a 32-pixel one), `x:` the pixel column it starts at (default 0), `scale:` 1–4 (default 1: each character 6 × 8 pixels with its gap, so 21 a row on 128 pixels; scale 2: 12 × 16 pixels, 2 lines tall, 10 a row), `length:` the characters (default as many as fit). Every printable ASCII character has a glyph, lower case too (the classic public-domain 5 × 7 font, `gm_text_glyph()` in `core/src/display.c`), and a `.` takes a place. Several text displays can share an OLED, one per line or side by side; the builder refuses ones that overlap. `chars:`, `segment_step:` and `segment_order:` are for segment displays only.
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
- **Generated code:** `GM_N_DISPLAYS`, `GM_DISPLAY_CHARS` (the widest display: the RAM is sized by it, 4 bytes a character a display), `GM_DISPLAY_SHOWS` and `GM_DISPLAY_TYPES` (bits of the `shows:` values and types used: only their code is built) and `GM_DISPLAY_MAP` in `gm_device_config.h`; `gm_displays[]` (`gm_display_t`: type, character count, first pins or the segment map, `shows`, and a text display's scale; its one "first pin" is the OLED hub and framebuffer byte it starts at, line × width + x) in `gm_device_config.c`.

```yaml
displays:
  - id: display
    type: segment14            # the default
    daw: param                 # optional: what a DAW script shows on it (time, track_name, param, status)
    chars:                     # left to right: each character's segment a
      - { tm1628: leds_a, number: 48 }
      - { tm1628: leds_a, number: 64 }
      - { tm1628: leds_a, number: 0 }
  - id: bank                   # 2 digits with their segments on a TM1628's grids
    type: segment7
    shows: preset_number
    segment_step: 16
    chars: [{ tm1628: leds_b, number: 0 }, { tm1628: leds_b, number: 1 }]
```

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
i2c: { sda: GPIO8, scl: GPIO9 }     # only if an I2C hub is used
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
- `delay` or `idle` with nothing turned on is a warning; so is `last_preset` on a device with one preset (there is nothing to remember, so it stays off).
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
- **Presets in use.** A fresh device has the listed presets in use; the other slots are free, and the preset buttons and keys step through only the ones in use (one preset: they do nothing). A host makes a new preset in the next free slot (PRESET_NAME or PRESET_SAVE) and deletes one with PRESET_DELETE: the presets after it move up a slot, so the free slots are always last. The count is the read-only global `preset.count`, stored with the globals (protocol/sysex.md §4.5 "Presets in use"). PRESET_MOVE reorders them: a preset moves to another slot and the ones between shift by one.
- **What a preset can set** is the mapping: `button.action`/`button.number` (`action`, `cc`, `note`, `preset`, `mcu`, `program`, `mmc`, `sysex`), `analog.cc`/`analog.mode` (`cc`, `mode`, `nrpn`, `rpn`, `sysex`), `encoder.cc`/`encoder.mode` (`cc`, `mode`, `nrpn`, `rpn`), `bank` (`preset.bank`, with `program`: "MIDI messages"), `pad.note`, and the per-preset globals above (`midi.channel`, `midi.transpose`, `midi.octave`, `velocity.curve`, `velocity.fixed`, `bend.glide`, `ramp.time`, `pad.channel`, `pad.curve`, `pad.aftertouch`, `pad.octave`), plus `program` (`preset.program`). The hardware side stays global and is refused in a preset, with where to set it: pins, LEDs, names, a control's `channel`/`min`/`max` and a button's `bank`, `adc_min`/`adc_max`, `takeover`, `steps_per_detent`, the key-time calibration (`key_matrix.velocity`), the pad sensing (`pad_settings` `threshold`, `release`, `full`, `window`, `retrigger`), `midi.transport`.
- **Power-up** loads the globals, then preset 1, or with `write_to_flash: { last_preset: true }` the preset used last ("Writes the device makes on its own"). A switch on the device stores nothing unless `last_preset` is on.
- **Storage.** The builder checks that every preset's worst-case stored record (every per-preset value changed, and a name) fits the platform's store, errors with the largest `slots` that fits, and warns above 85% (tools/builder/README.md "Storage budget"). The store keeps records in `include/config_store.h`'s v2 format (2 words a value), except on the STM32F1, which packs them (v3: one group per param, a byte or a halfword a value, `platform/stm32f1/src/config_store.c`) to fit its 3 config pages of 1 KB: there the Code 61 fits up to 5 slots.

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

A mode is a layer that turns the keys (and buttons) into function keys: while it is active, a key press runs what the mode binds to that key and sends no note (research/core/presets-design.md §6). It is how a keyboard with few buttons switches presets, the octave or the channel. Modes are device UI, so they are the same in every preset: a preset can't take away the way back.

**No keyboard needed.** A device with buttons and no `key_matrix` (a pad box, a fader box, a floor board) has modes too: toggle and hold modes with `buttons:`, `with:` (Shift) combinations, layers ("Button layers") and cycles ("Cycles"). Only `keys:` needs a key matrix, and with it the sub-modes (`parent:`), which are entered from a key. A mode needs a button to enter it (`toggle:`, `hold:` or `cycle:`).

```yaml
# an FCB1010-style floor board: sw1 and sw2 send programs 1 and 2 (action: program)
modes:
  - id: bank_b
    toggle: bank                 # a layer-only toggle: programs 6 and 7 while it is on
    layer: { sw1: { program: 6 }, sw2: { program: 7 } }
  - id: banks
    cycle: page                  # each press: the next step
    steps:
      - { id: live, name: LIVE }
      - { id: fx, name: FX, layer: { sw1: { action: cc_toggle, cc: 80 } } }
  - id: setup
    hold: tuner                  # while Tuner is held, the switches pick the channel
    buttons: { sw1: channel_down, sw2: channel_up }
```

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
  - id: channel
    hold: { button: channel }    # active while the button is held (or `hold: channel`)
    buttons:                     # what these buttons do instead, while the mode is active
      up: channel_up
      down: channel_down
```

- **Entering and leaving.** The toggle button enters the mode and, pressed again, leaves it, also from one of its sub-modes; there is no timeout. A `hold:` mode is active only while its button is held (it can't have sub-modes); it can be held during a toggled mode, and goes first.
- **Which modes can be on together.** The *key modes*, toggle modes with `keys:`, `buttons:` or sub-modes, take turns: entering one leaves the other. A *layer-only toggle mode* (a toggle mode with only a `layer:`, "Button layers") and each cycle ("Cycles") have their own state, so any number of them can be on at once, with each other and with a key mode or a held hold mode. Keys held when the mode starts get their Note Off then, and their release sends nothing. A sub-mode (`parent:`) goes back to its parent after one action, so Edit → E3 → D4 picks preset 1 and leaves you in edit mode for another action, until Edit is pressed again.
- **`toggle: { button: <id>, with: <id> }`** toggles the mode only while the `with:` button is held (Shift + Button mode, say). A plain press runs the toggle button's own action, so it keeps its `action:` and presets can change it; a press that toggled the mode sends nothing on release. `with:` is for `toggle:` only.
- **A modifier** is a `hold:` mode with only a `layer:` (no `keys:` or `buttons:`), like Shift: its button keeps its own action (presets can change it), and while it is held the layer's twins take their buttons' presses, so each twin is that button's "Shift +" action ("Button layers"). It can also be the `with:` of toggles and cycles: Shift + Button mode still toggles, so a button that the modifier toggles a mode with can't have a twin in its layer. Its button can't be bound in a mode, have a twin or enter another mode. Two hold modes held at once: the last pressed is live, and letting go of it goes back to the other.

```yaml
modes:
  - id: shifted
    hold: shift                  # Shift still sends its CC (and keeps its daw: role)
    layer:
      up: { action: program_next }     # Shift + Up: the next program
      down: { action: program_prev }
```

- **The toggle (or hold) button** has no action of its own (leave `action:` out): it only toggles the mode, and presets can't rebind it. A modifier's button is the exception (above). A `button.action` set for it from the host is stored but not used. Its presses still show as EVENTs. Its LED is lit while the mode is active (the default `led:`).
- **While a mode is active,** unbound keys are silent, and the buttons keep their normal actions. A mode with no `keys:` leaves the keyboard playing.
- **Buttons.** `buttons:` maps button ids to the same actions as keys (not sub-modes or ranges). While the mode is active, a bound button runs that instead of its own action; its release sends nothing, even if the mode has ended by then. A button pressed before the mode started is released normally. A mode's own toggle/hold button can't be bound.
- **Key actions:** `preset_select` (`preset: N`), `preset_next`, `preset_prev` (these need 2 or more preset slots), `octave_down`, `octave_up`, `octave_reset`, `transpose_down`, `transpose_up`, `transpose_reset`, `channel` (`channel: 1..16`), `channel_up`, `channel_down` (16 wraps to 1), `velocity_curve` (`curve: normal | soft | hard | fixed`, velocity matrices only), and `{ mode: <id> }` to go to a sub-mode.
- **Ranges.** `keys:` may also hold one range, `white_from: <key>` (white keys only) or `from: <key>` (every key), with `action: preset_select` or `channel` and an optional `count:` (default: every slot, or 16 channels). The keys are numbered 1, 2, ... in order. Single keys can sit beside a range in the same mapping.
- **What it sends.** A preset switch from a mode is the same as from a button: Note Offs, PRESET_CHANGED and PARAM_CHANGED `preset.active`, and the preset's Program Change. Octave/transpose keys send PARAM_CHANGED like the octave buttons. `channel`, `channel_up`, `channel_down` and `velocity_curve` send PARAM_CHANGED and make the preset dirty, like a host edit: COMMIT keeps the change, a preset switch drops it. A display shows the preset's name or `CH <n>` ("Displays").
- **`layer.enabled`** (one per mode, in the "Layers" group, global) is there on a device with layer twins or a cycle. 0: the mode can't be entered (its button is as if it weren't there, so a `with:` press is the button's own action; a mode that is on can still be left), and a cycle skips that step. Defaults are true, a step's `enabled:` aside.
- **The builder checks** that ids are unique (mode, cycle and step ids together), that every mode has exactly one of `toggle:`, `hold:`, `parent:` or `cycle:`, that bound buttons exist and don't enter a mode, that parents exist and don't loop, that every sub-mode is reachable from a key, that `keys:` and sub-modes have a key matrix to use, that keys are on the keyboard and bound once per mode, and the actions' numbers.

**Generated code** (`gm_device.h` `gm_mode_t`, only with `modes:`): `GM_N_MODES` in `gm_device_config.h` (0 without modes: the mode code is then left out; without a key matrix, `GM_HAS_MATRIX 0`, the key half of it is: the key table lookups and the Note Offs for held keys), and `gm_modes[GM_N_MODES]`, with one key table per mode sorted by key, `{ key index, GM_MK_* op, arg }`, and one button table per mode with `buttons:`, `{ button index, op, arg }` (`hold`, `n_buttons`, `buttons` are the struct's last fields, so a config generated before them still builds), then `with`, `cycle`, `colour`, `name` ("Cycles"; `name` is the step's name, else the mode's id, the label of `layer.enabled`) and `modifier` (1: a modifier). `GM_MODES_ENABLED` lists the `layer.enabled` defaults. The runtime keeps 2 bytes for the active and held modes, 8 for the buttons pressed in a mode and 6 for the preset blink; with layers or cycles 24 more (the layer-only toggles, what EVENT 8 last said) and one per cycle.

### Button layers

A root mode (`toggle:` or `hold:`) can give buttons a second meaning while it is active: a **layer**. Each button in `layer:` gets a **twin**, a button of its own on the same switch, with its own action, CC or note, toggle state, DAW role and LED look, and the button's LED pins.

```yaml
buttons:
  - { id: b2, pin: ..., action: cc_toggle, cc: 86, led: b2_led, daw: mute_1 }
leds:
  - { id: b2_led, pin: ..., pin2: ..., pin3: ..., idle: white, active: blue }
modes:
  - id: arm
    toggle: { button: button_mode, with: shift }
    led: button_mode_led
    layer:
      b2: { cc: 52, daw: arm_1, active: red }   # B2 in the arm layer: CC 52, red while on
```

- **What a twin leaves out is the button's:** `action:` (so `cc:` alone keeps a `cc_toggle` a toggle), `cc:` / `note:` / `preset:`, and the LED looks. `name:` defaults to "<button name> <mode id>" ("B2 arm"). `daw:` is the twin's own, or none.
- **One live at a time.** While the mode is active the twin takes the button's presses and drives the LED; otherwise the button does. A press's release goes to whichever took the press, even if the layer changed in between. When layers that are on at once have twins of the same component, the first of these wins: the held hold mode, the toggled key mode, a cycle's current step, then layer-only toggles; between steps of different cycles, or between layer-only toggles, the one earlier in `modes:`.
- **Each keeps its state.** Toggling B2 in the arm layer leaves its mute state alone; switching layers redraws the LED with the live one's state.
- **They are ordinary components:** the twin is the next button after the YAML's (id `<button>_<mode>`, e.g. `b2_arm`) and its LED the next LED (`b2_led_arm`). A host's echo of the twin's CC sets its toggle state, and GET_COMPONENT lists them with their roles, so DAW scripts see `arm_1` next to `mute_1` and drive each LED as usual. Their `layer` byte (GET_COMPONENT, kinds 1, 4, 5 and 7) is the mode index + 1 (0 for the YAML's own components): a twin is live only while EVENT 8 says its mode is on.
- **Their settings are global**, the same in every preset, stored once with the global settings: `layer.button.action` and `layer.button.number` (one per button twin, labelled with the twin's name), `layer.encoder.cc` and `layer.encoder.mode` (one per encoder twin), in the config's "Layers" group. `button.*` and `encoder.*` cover only the YAML's own buttons and encoders, and stay per preset; presets: can't set a twin. A preset switch leaves the twins' settings alone (and drops toggle states, as for every button).
- **EVENT 8 (layer)**, while `debug.events` is 1: index = mode index, value 1 when a mode's layer turns on (a toggle, a hold mode's button, a cycle step), 0 when it turns off; the offs of one change come first. Every time the host sets `debug.events` to 1 (also when it already is), the device sends EVENT 8 value 1 for each mode that is on, so a script learns the state. Modes are reported whether or not they have twins (a cycle step without a layer too); sub-modes aren't.
- **The builder checks** that layers are on root modes, that a button isn't both in the mode's `buttons:` and its `layer:`, that plain `toggle:` and `hold:` buttons, modifiers and the `with:` buttons have no twin, that a modifier's layer has no twin of a button it already toggles a mode with, and that a twin isn't given a twin.
- **A `toggle: { button, with }` button may have a twin**, in its own mode's layer or another's: Shift + the twin toggles the mode just as Shift + the button does (the mode's toggle is the button or any of its twins), and the twin alone is its own action. The Code 61's Pad octave - is the `pads` mode's toggle and, in that layer, `grid_left`.

**Pads and encoders** can be in a layer too, by id, next to buttons:

```yaml
modes:
  - id: pads
    toggle: { button: pad_octave_down, with: shift }
    layer:
      pad_13: { daw: clip_1_1, idle: off }    # a pad twin: name, daw, idle, active
      pad_octave_down: { daw: grid_left }
  - id: pan
    toggle: { button: encoder_mode, with: shift }
    led: encoder_mode_led
    layer:
      encoder_1: { cc: 41, daw: pan_1 }       # an encoder twin: name, cc, daw
```

- **A pad twin** plays no note. While its layer is active, the physical pad's hits are the twin's: an EVENT type 7 with the twin's pad index and the peak (`debug.events` on), 0 on release, and its LED is the twin LED: the host's state (FB_LED, FB_LED_RGB), lit with its active look while the twin's hit is held. The pad's own note, LED and state stay with the pad; sensing stays per physical pad (no extra ADC reads or pins), and a hit's release goes to whichever took the hit. Twins come after the YAML's pads (GET_COMPONENT kind 7, id `<pad>_<mode>`, its LED `<pad LED>_<mode>`); they have no `pad.note`. Keys: `name`, `daw`, `idle`, `active`.
- **An encoder twin** is the next encoder after the YAML's (id `<encoder>_<mode>`), with its own `layer.encoder.cc` (`cc:`, else the encoder's) and `layer.encoder.mode` (it starts as absolute). `cc: none` (`layer.encoder.cc` -1) sends no CC at all, only its EVENTs (type 4, `debug.events` on): for a DAW script's knob. The encoder is decoded once, in the scan; while the layer is active its turns go to the twin: its CC, mode, absolute value and EVENT index, and the encoder's own value is kept for when it comes back. An encoder with `ring:` / `ring_center:` can't have a twin yet. Keys: `name`, `cc`, `daw`.
- **Cost:** the twins' params are globals: they count once in the store's worst case, not per preset slot (the Code 61's 24 encoder and 14 button twins and 7 modes: 128 B of the global record).

**Generated code:** `gm_button_t` / `gm_led_t` / `gm_encoder_t` `.layer` (mode index + 1; 0 = not a twin) and `.base` (the button / LED / encoder it is a twin of), `gm_pad_twins[]` (`gm_pad_twin_t`: layer, base pad, LED), `gm_mode_t.with` (button index + 1, 0 = none), and in `gm_device_config.h` `GM_LAYERS` (the number of twins), `GM_PAD_TWINS`, `GM_BTN_TWINS` (the last `GM_BTN_TWINS` buttons are twins) and `GM_ENC_TWINS` (the last `GM_ENC_TWINS` encoders are twins and aren't scanned); the twins' values are the tails of `gm_cfg`'s button and encoder arrays, under the `layer.*` params. Without twins or cycles the layer code is left out.

### Cycles

A **cycle** is a group of root modes, its **steps**, taken in turn by one button: exactly one step is on at a time, and each press of the button moves to the next enabled one. Each step is a mode of its own, with its own layer.

```yaml
modes:
  - id: encoders
    cycle: { button: encoder_mode, with: shift }   # Shift + Encoder mode: the next enabled step
    led: encoder_mode_led                            # shows the step's colour
    steps:
      - { id: enc_midi, name: MIDI, colour: blue }   # no layer: the components as they are
      - id: enc_zyn
        name: ZYN
        colour: cyan
        layer:
          encoder_1: { cc: none, daw: zynthian.zynpot_1 }
          bank_left: { daw: device_bank_prev }
      - { id: enc_pan, name: PAN, colour: red, enabled: false, layer: { encoder_1: { cc: none, daw: pan_1 } } }
```

- **`cycle: { button, with }`** (`with:` optional; or `cycle: <button id>`) instead of `toggle:` / `hold:` / `parent:`. Each press (with `with:` held, if given) moves to the next enabled step, round the end, skipping disabled ones; with only one enabled, the press shows its name again. With `with:`, a plain press is the button's own action, as for `toggle:`; without it, the button has no action of its own.
- **Steps** (2 to 8): `id`, `name` (up to 16 characters; the display shows it, as much as fits, when the step is switched to; default: the id), `colour` (an LED look for the cycle's LED, resolved for its kind like `led_defaults:`, or a full colour, `"#ff8000"` or `blue 20%`: as it is on an LED that can show any colour, the nearest on/off colour elsewhere; default: its active look), `enabled` (default true) and `layer:` (as "Button layers"). No `keys:` or `buttons:`. A step is a root mode with its own mode index, in `modes:` order where the cycle is, so its twins' `layer` is that index + 1.
- **The LED** (`led:`, default the button's LED) shows the current step's colour all the time, over a host's level.
- **Power-on** starts each cycle at its first enabled step; the step isn't remembered across power-off. A preset switch doesn't change it.
- **Disabled steps** (`layer.enabled` 0, "Modes"; the YAML's `enabled:` is its default) are skipped. With every step of a cycle disabled the first step counts as enabled. Changing `layer.enabled` takes effect at the next press.
- **Switching** runs the layers' update, the display shows the new step's name, and EVENT 8 says 0 for the old step and 1 for the new one.
- **Independent** of every other mode: a cycle, layer-only toggles and a key mode can all be on together ("Modes").
- **The builder checks** that a cycle has 2 to 8 steps, that step ids are unique among all modes, that its button doesn't enter another mode (toggle, hold or cycle), that `colour` is a look of the cycle's LED (a look another kind of LED has is a warning: the nearest it can show), that the cycle has an LED for its colours, and that at least one step is enabled.

**Generated code:** each step is a `gm_modes[]` row with the cycle's button as `toggle` / `with`, `cycle` = the cycle's number + 1, `colour` = 0x80 | the LED pins to light (bit 0 pin, 1 pin2, 2 pin3; 0 = the active look) and `name`; `GM_N_CYCLES` in `gm_device_config.h`. A step with a full colour on an LED that can show it also has `gm_mode_rgb[]` (`GM_LED_RGB(0xRRGGBB)`, 0 for the others; `GM_MODE_RGB 1`), and `colour` is its nearest on/off colour. The runtime keeps one byte per cycle (its current step).

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

LED `feedback:` takes exactly one of `note:` / `cc:` (a number 0..127, with an optional `channel: 1..16`), `clock: beat`, `transport: playing`, or `mcu: <MCU LED>` (below). Values map to the LED as in Mackie Control: 0 off, 1 blinking, anything else on (the first colour of a bicolour LED). DAW scripts (F3) can also reach a bicolour LED's second colour, and set an RGB LED's colour (FB_LED_RGB). An RGB LED (`pin3:`) shows its active look (white unless `active:` says otherwise) for any level from its button, plain feedback, Mackie Control or FB_LED, and its idle look for 0 ("LED looks"); only FB_LED_RGB picks a colour: an LED of a full-colour hub (`ws2812`, `maudio_pad_leds`) shows it as given (each channel 0..127 sent as 0..255), any other RGB LED with each channel on or off (on if it is at least half the brightest channel: 127/60/0 is red and green).

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
`gm_device_config.c`: `const gm_device_t gm_device = {...}` with its tables, and the default arrays `gm_button_action_defaults[]`, `gm_button_number_defaults[]`, `gm_analog_cc_defaults[]`, `gm_encoder_cc_defaults[]` (each at least one element long, so a count of 0 still compiles). Only when the YAML uses them: `gm_led_fb[]` (`GM_LED_FB`), `gm_analog_pickup[]` (`GM_ANALOG_PICKUP`), `gm_analog_mcu[]`, `gm_encoder_mcu[]`, `gm_encoder_ring[]` (`GM_MCU`), `gm_enc_accel[]` (`GM_ENC_ACCEL`, "Encoder acceleration"), the "MIDI messages" tables (`gm_button_ctl[]`, `gm_analog_ctl[]`, `gm_encoder_ctl[]`, `gm_button_bank[]`, `gm_encoder_mode_defaults[]`, `gm_sysex[]`). Plain `make` (firmware and `core/test`) generates them from `devices/keystation-mini32-mk3.yaml` into the build dir.
