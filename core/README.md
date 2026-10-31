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
| `include/gm_device.h` | the hardware model: virtual pins, hubs, buttons, LEDs, analog inputs, encoders, key matrix |
| `include/gm_hal.h` | what a platform provides |
| `include/gm_controller.h`, `src/controller.c` | the runtime: scanning, actions, velocity, LEDs, MIDI out |
| `src/vpin.c` | virtual pins: MCU pins via the HAL, expander hubs in software |
| `src/settings.c` | the param table, sized from the device's component counts |

Rules: C11, no heap, no floating point, no libc beyond `<stdint.h>` (the NUC120 has 4 KB of RAM and 32 KB of flash). Headers are C++-safe (the ESP platform is Arduino C++).

## Virtual pins and hubs

Every pin in a device is a `gm_pin_t { hub, flags, num }`:
- `hub = 0`: a pin of the MCU. `num` is the platform's pin number: NUC1xx `port * 16 + bit` (`PA0` = 0, `PB12` = 28, `PC7` = 39); ESP32/ESP8266 the GPIO number.
- `hub = n ≥ 1`: pin `num` of hub `n - 1` (chained shift registers continue: pins 8–15 are on the second chip).
- `flags`: `GM_PIN_INVERT` (active-low: pressed / lit = electrical 0), `GM_PIN_PULLUP`, `GM_PIN_PULLDOWN` (MCU pins; `pullup` also works on MCP23017 pins, which have internal pull-ups).

Hub types:

| Type | YAML `type` | Pins it uses | Gives |
|---|---|---|---|
| `GM_HUB_HC595` | `74hc595` | `data` (SER), `clock` (SRCLK), `latch` (RCLK), optional `enable` (/OE); `chain` chips | 8 × chain outputs. LEDs, matrix rows |
| `GM_HUB_HC165` | `74hc165` | `data` (QH), `clock` (CLK), `latch` (SH/LD); `chain` chips | 8 × chain inputs. Buttons, matrix columns |
| `GM_HUB_MUX` | `74hc4051` (3 select), `74hc4067` (4 select), or `mux` with `select:` | `select` pins, `common` pin, optional `enable` (/E), `settle_us` | 8 / 16 input channels, analog (if `common` is an ADC pin) or digital |
| `GM_HUB_MCP23017` | `mcp23017` | I2C (`address`, the platform's I2C pins) | 16 GPIO |
| `GM_HUB_PCF8574` | `pcf8574` | I2C (`address`) | 8 GPIO (quasi-bidirectional: inputs are written high) |

Inputs on HC165, MCP23017 and PCF8574 are read once per scan into a cache, and outputs on HC595 are written once per scan, so a whole key matrix can live on shift registers.

## Components

| Component | Fields | Params it creates (per item, as arrays) |
|---|---|---|
| button | `pin`, `label`, `led` (optional), default `action` + `number` | `button.action`, `button.number` |
| LED | `pin`, optional `pin2` (bicolour), `label` | – (LEDs show their button's state, or the octave/transpose level) |
| analog (pot, slider, fader, expression) | `pin` (ADC pin or mux channel), `label`, `raw_min`, `raw_max`, default `cc` | `analog.cc` |
| encoder | `a`, `b`, `label`, `steps_per_detent`, default `cc` | `encoder.cc`, `encoder.mode` |
| key matrix (at most one) | `rows`, `cols`, `kind` (`switch` / `velocity`), `keys`, `first_note`, `settle_us`, and for `kind: velocity` optional default key times `velocity: { fast_ms, slow_ms }` (time between the two contacts for velocity 127 and 1; default 1.2 and 120 ms) | the velocity params if `kind: velocity` |

Global params (always present): `midi.channel`, `midi.transpose`, `midi.octave`, `bend.glide`, `ramp.time`, `debug.events`, `debug.log_level`. The list with ids is in `src/settings.c`; ids never change meaning.

**Key matrix.** One row is driven at a time (to the active level: high, or low with `inverted`), the columns are read, then the row is released. `kind: switch`: key = row + rows × col. `kind: velocity`: two contacts per key; key k is on row k % rows and columns 2p, 2p+1 with p = k / rows. Whichever contact closes first starts the timer, the second sends Note On with a velocity from the time between them, and the key is released when both are open. Put diodes on the matrix if more than one key can be held.

**Button actions** (`button.action` values; YAML names in brackets): 0 none (`none`), 1 CC momentary (`cc_momentary`), 2 CC toggle (`cc_toggle`), 3 CC ramp (`cc_ramp`: glides to 127 while held, back to 0 on release), 4 note (`note`), 5 octave down (`octave_down`), 6 octave up (`octave_up`), 7 transpose down (`transpose_down`), 8 transpose up (`transpose_up`), 9 pitch bend down (`bend_down`), 10 pitch bend up (`bend_up`), 11 program next (`program_next`), 12 program previous (`program_prev`). `button.number` is the CC or note number.

**Encoder modes** (`encoder.mode`): 0 absolute (0..127, sends the value), 1 relative two's complement (1 = +1, 127 = −1), 2 relative offset (65 = +1, 63 = −1).

## Device YAML

```yaml
gemifi: 1                      # format version
name: Keystation Mini 32
version: 0.1.3                 # firmware version (major.minor.patch): HELLO, .gfw header; default 0.1.0
board_id: 0x0001               # protocol/sysex.md section 7; 0x3F00-0x3FFF = DIY/private
platform: nuvoton-nuc1xx       # nuvoton-nuc1xx | esp32 | esp32s2 | esp32s3 | esp8266
chip: NUC120LC1BN              # NUC: part; ESP: PlatformIO board id (e.g. esp32-s3-devkitc-1)
usb: { vid: 0x1209, pid: 0x0002 }   # native-USB platforms
midi: { transport: usb }       # ESP only: usb (S2/S3) | serial | ble (ESP32)
i2c: { sda: GPIO8, scl: GPIO9 }     # only if an I2C hub is used
scan_us: 125                   # matrix/encoder scan period
boot_combo: [Pitch down, Pitch up]  # button labels

hubs:
  - id: faders
    type: 74hc4051
    select: [GPIO4, GPIO5, GPIO6]
    common: GPIO1
    settle_us: 5
  - id: leds
    type: 74hc595
    data: GPIO10
    clock: GPIO11
    latch: GPIO12
    chain: 2

buttons:
  - label: Sustain
    pin: { pin: PA1, inverted: true }
    action: cc_momentary
    number: 64
    led: Sustain LED

leds:
  - label: Sustain LED
    pin: { pin: PA12, inverted: true }
  - label: Oct - LED
    pin: { pin: PA10, inverted: true }
    pin2: { pin: PA14, inverted: true }

analog:
  - label: Volume
    pin: PA0
    cc: 7
  - label: Fader 1
    pin: { hub: faders, channel: 0 }
    cc: 20

encoders:
  - label: Encoder 1
    a: { pin: GPIO20, pullup: true }
    b: { pin: GPIO21, pullup: true }
    cc: 30
    steps_per_detent: 4

key_matrix:
  kind: velocity
  rows: [PA8, PA9, PC0, PC1, PC2, PC3, PC6, PC7]
  cols: [PB0, PB1, PB2, PB3, PB4, PB5, PB8, PB12]
  keys: 32
  first_note: 48
  settle_us: 1
  velocity: { fast_ms: 1.6, slow_ms: 120 }
```

A **pin** is a string (`PA1`, `GPIO5`, `5`) or a mapping: `{ pin: PA1 }` or `{ hub: <hub id>, pin: 3 }` (also `channel:` for a mux), plus `inverted`, `pullup`, `pulldown`. Labels must be unique per component kind; buttons refer to LEDs and `boot_combo` to buttons by label.

## Generated code (tools/builder output)

`gm_device_config.h`:
```c
#define GM_FW_VERSION 0x00010003u      /* from version: (major << 24 | minor << 16 | patch) */
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
#define GM_DEVICE_NAME "Keystation Mini 32"
#define GM_USB_VID 0x1209
#define GM_USB_PID 0x0002
#define GM_HAS_I2C 0
#define GM_PLATFORM "nuvoton-nuc1xx"
/* ESP only: */
#define GM_MIDI_TRANSPORT GM_MIDI_USB     /* GM_MIDI_USB / _SERIAL / _BLE, values in gm_hal.h */
#define GM_I2C_SDA 8                      /* GPIO numbers, when i2c: is given; */
#define GM_I2C_SCL 9                      /* otherwise the board's default Wire pins */
```
Optional build flags on ESP: `GM_MIDI_BAUD` (serial transport, default 115200), `GM_FW_VERSION` (`major << 24 | minor << 16 | patch`; normally set by the YAML `version:`).
`gm_device_config.c`: `const gm_device_t gm_device = {...}` with its tables, and the default arrays `gm_button_action_defaults[]`, `gm_button_number_defaults[]`, `gm_analog_cc_defaults[]`, `gm_encoder_cc_defaults[]` (each at least one element long, so a count of 0 still compiles). Plain `make` (firmware and `core/test`) generates them from `devices/keystation-mini32-mk3.yaml` into the build dir.
