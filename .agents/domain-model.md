# Domain model

What gemifi is made of: its concepts, who may change them, and what always holds. This file lives at
`.agents/domain-model.md` and describes the code **as it is on this branch**. Proposed changes are `M`
items in `.agents/coding-standards-todos.md` and in the pull request that makes them; they are written in
here by the commit that makes them true. Use these terms, and only these, in code, tests, docs and
reviews.

## Purpose

gemifi is "ESPHome for MIDI controllers": a YAML file describes a controller's hardware (an M-Audio
Code 61 or Keystation, a DIY ESP32 box), the builder turns it into firmware, and the device then sends
MIDI and can be tuned and updated over SysEx from the web updater, a host tool or a DAW script.

## Flow

```
device YAML ─builder─▶ device config (C tables) ─┐
core (runtime) + platform (HAL) ─────────────────┴─▶ firmware image ─▶ device

device:  inputs ─scan─▶ controls ─actions─▶ MIDI out ──▶ host
         host ─MIDI in─▶ feedback / SysEx protocol ─▶ LEDs, displays, params, presets
```

- **Build:** the builder validates the YAML, writes `gm_device_config.h/.c`, and builds the platform's
  firmware project with it (make on NUC1xx and STM32F1, PlatformIO + ESP-IDF on ESP32).
- **Play:** the scan reads inputs through pins; the controller turns changes into MIDI messages with
  the control's settings (params) and the active preset; LEDs and displays show state.
- **Feedback:** a host lights LEDs, writes display text, moves encoder rings (F1 / F3 / Mackie
  Control), and reads or changes params, presets and firmware over the SysEx protocol.
- Dependencies point from specific to general: platform → core HAL interface (`gm_hal.h`); generated
  config → core types (`gm_device.h`); core never includes a platform or a device header except the
  generated `gm_device_config.h`.

## Actors

| Actor | Is | Reaches the system through | May change |
|---|---|---|---|
| Device author | whoever writes the device YAML | the builder (CLI or dashboard) | the hardware description and defaults, at build time |
| Player | the person playing the controller | its buttons, keys, pots, modes | live state: octave, transpose, channel, active preset, layers |
| Host | the web updater, `gemifi.py`, a DAW script or Mackie Control host | USB/BLE/serial/DIN MIDI: SysEx protocol, feedback messages | params, presets, display text and LEDs it owns, firmware (when unlocked) |
| Platform | the chip's HAL and its interrupts | `gm_hal.h` calls, the scan tick | nothing in the model: it only reports levels, readings and bytes |

## Lifecycles

### Device config

```
YAML --validate (builder)--> checked model --generate--> gm_device_config.h/.c --build--> image
```

The config hash (`config.hash`) ties a stored profile to the config it was made for.

### Hub state (per hub, at run time)

```
power-up --vpin_init / ops.init--> idle --first write--> chip set up --send/flush--> in step with RAM
```

A bus hub (I2C) is refreshed and flushed from the main loop; a scan hub (shift registers) from the scan
tick; a bit-banged LED driver sends a budget of bytes per main-loop pass.

### Display content

```
own (shows:) --FB_TEXT (host)--> host text --device news--> news over host text (1.5 s) --> host text
host text --FB_RELEASE--> own
```

### Preset

```
stored slot --select (button, mode, host)--> active --edit (host)--> dirty --save/commit--> stored
```

## Invariants

| Invariant | Enforced by |
|---|---|
| Every pin used by the YAML exists on its MCU or hub, and two uses of one pin are allowed only with `allow_other_uses` on each | builder, `gemifi_device.py` pin resolution |
| Only the drivers of hub types the device uses are linked | `GM_HUB_TYPES` bits + `USES()` in `core/src/vpin.c` `hub_ops[]` |
| A bus (I2C) hub is never touched from the scan interrupt | `GM_HUBF_BUS`, `vpin_refresh_i2c` / `vpin_flush_i2c` called from the main loop only |
| Text displays on one screen never overlap | builder, `validate_text_display` |
| No heap, no floating point, no libc beyond `<stdint.h>` in core | convention (`core/README.md`), the NUC120's 4 KB RAM / 32 KB flash |
| A param id never changes meaning | convention, `core/src/settings.c` id list |

## Glossary

### Building a device

- **Device YAML (device file):** the text file that describes one controller: its chip, pins, hubs,
  controls, LEDs, displays, presets and modes. Owner: `devices/*.yaml`; format in `core/README.md`
  "Device YAML", JSON schema `site/src/schema/device.json`.
- **Builder:** the tool that checks a device YAML, generates its C tables and builds the firmware;
  also the dashboard. Owner: `tools/builder/` (`gemifi_build.py`, `gemifi_device.py`, `dashboard.py`).
- **Platform:** the chip family a device runs on (`nuvoton-nuc1xx`, `stm32f1`, `esp32`, `esp32s2`,
  `esp32s3`) and its hardware layer. Owner: `platform/<family>/`; `Platform` subclasses in
  `gemifi_device.py`.
- **Board profile:** what a specific board adds to its platform: flash layout, clock, reserved pins.
  Owner: `platform/<family>/boards/`, `gemifi_board.py`.
- **Device config:** the generated C tables a firmware is built with. Owner: `gm_device_config.h/.c`,
  written by `gemifi_device.py`; types in `core/include/gm_device.h` (`gm_device_t`).
- **Package / substitution:** a reusable YAML fragment, and a `${name}` value set at build time.
  Owner: `gemifi_yaml.py`.

### Hardware

- **Component:** one kind of part a device can have, in a folder of its own: its C code, its YAML
  (validation and generated C), its docs and its tests. A device builds only the components it uses.
  A base component defines what the components on it implement (`hub`, `display`). Owner: `components/<name>/`
  (`__init__.py`, `<name>.c/.h`, `README.md`, `test/`); found by `components/__init__.py`, listed per
  device in the generated `gm_components.h`, `gm_components.mk` and `gm_components.cmake`. So far `hub`,
  `ws2812`, `display` and `ssd1306`; the other hub types are still in `core/src/vpin.c` and
  `gemifi_device.py` (R5).
- **Pin:** one input or output line: an MCU pin, or a pin of a hub. Owner: `gm_pin_t`
  (`core/include/gm_pin.h`: `hub`, `flags`, `num`).
- **Hub (expander, chip):** a chip on the board, beyond the MCU, declared as a top-level YAML list named
  after the chip (`sn74hc595:`, `mcp23017:`, `ws2812:` ...), that gives the device pins (shift
  registers, I/O expanders, multiplexers, decoders, switch matrices, XY pads, LED drivers, LED strips).
  Owner: the `hub` base component (`components/hub/`:
  `HubType`, `gm_hub.h`); `gm_hub_t` (config union, `gm_device.h`, extended by `GM_HUB_CONFIG_MEMBERS`),
  `gm_hub_state_t` (extended by `GM_HUB_STATE_MEMBERS`); each type in its component
  (`components/ws2812/`) or still in `core/src/vpin.c` and `gemifi_device.py` (`HUB_DEFS` holds both).
- **Hub driver:** the code for one hub type behind the hub ops interface (`init`, `read`, `write`,
  `analog`, `rgb`, `refresh`, `flush`, `send`, `poll`). Owner: `gm_hub_ops_t` (`components/hub/gm_hub.h`),
  `hub_ops[]` in `vpin.c` (component types through `GM_HUB_OPS`). What the base gives its types: the
  hub RAM (`gm_hub_ram`, `gm_hub_ram_alloc`) and the full-colour output (`gm_hub_colour_out`).
- **Screen (YAML `display:`):** a panel text displays draw on, declared ESPHome style as a `display:` item
  on a platform: `display: [{ id: oled, platform: ssd1306, address: 0x3C }]`. Owner: the `display` base
  component (`components/display/`: `ScreenType`, `gm_screen.h`, `screen.c`: the framebuffers, drawing,
  sending within the bus budget) and each platform's component (`components/ssd1306/`: its config and
  `gm_screen_ops_t`); `gm_screens[]` (generated), `GM_N_SCREENS`, `GM_SCREEN_RAM`, `dev.screens` in the
  builder. In C it is a "screen" because `gm_display_t` is the line of text.
- **Virtual pin (vpin):** the core's pin dispatch: a pin's read or write goes to the MCU or its hub's
  driver. Owner: `core/src/vpin.c`, `core/include/gm_vpin.h`.
- **I2C bus:** the one I2C bus a device may have (`i2c: { sda, scl }`), shared by the I2C hubs and screens.
  Owner: `hal_i2c_write` / `hal_i2c_read` (`gm_hal.h`, no bus argument), `GM_I2C_SDA/SCL`.
- **Scan:** reading inputs on a timer (`scan_interval`): the key matrix, switch matrices, encoders and
  scan hubs, in software or with timer + DMA (hardware scan). Owner: `gm_scan_tick`, `gm_scan_hw`
  (`controller.c`), `vpin_scan_*`.

### Controls

- **Button:** a switch with an action (CC momentary/toggle/ramp, note, octave, transpose, program,
  preset, MMC, SysEx ...). Owner: `gm_button_t`; `controller.c` "buttons".
- **LED:** a light on one, two or three pins (single, bicolour, RGB), showing its button, a level or
  what a host sends; looks for idle and active. Owner: `gm_led_t`; `controller.c` "LEDs".
- **Analog input (pot, fader, wheel):** a reading sent as CC, 14-bit, NRPN/RPN, pitch bend or pressure.
  Owner: `gm_analog_t`; `controller.c` "analog inputs".
- **Encoder:** a rotary encoder sending absolute or relative values, with acceleration and an LED ring.
  Owner: `gm_encoder_t`; `controller.c` "encoders" (scan side and main side), "encoder rings".
- **Key matrix:** the keyboard: rows × columns of switch or velocity contacts. At most one. Owner:
  `gm_matrix_t`; `controller.c` "key matrix", "velocity".
- **Pad:** a velocity pad read from pressure. Owner: `gm_pad_t`; `controller.c` "pads".
- **Control twin:** a second copy of a control on a layer. Owner: `gm_pad_twin_t` and the layer tables.

### Behaviour

- **Action:** what a button does when pressed. Owner: `button.action` param values.
- **MIDI message (kind):** what a control sends (CC, note, NRPN, SysEx string ...). Owner:
  `controller.c` "MIDI messages", `gm_ctl_t`, `gm_sysex_t`.
- **Mode / layer / cycle:** a held or toggled state that changes what controls do (Shift, a button
  layer, a cycle of values). Owner: `gm_mode_t`, `gm_mode_key_t`; `controller.c` "modes".
- **Param:** a setting a host can read and change at run time (`button.action`, `midi.channel` ...).
  Owner: `core/src/params.c`, `core/src/settings.c`.
- **Preset:** a stored set of param values, in slots, switched by buttons, modes or a host. Owner:
  `core/src/presets.c`.
- **Display:** a line of characters the device shows text on: segment characters on hub output pins
  (`segment14`, `segment7`) or text on a screen (`text`, `display: <id>`). It shows its own content (`shows:`), the
  device's messages, or a host's text. Owner: `gm_display_t`, `core/src/display.c`,
  `core/include/gm_display.h`.
- **DAW role:** what a control or display is for to a DAW script (`daw: device_param_1`, `param`,
  `track_name` ...). Owner: GET_COMPONENT roles, `gm_comp_t`.

### Talking to hosts

- **SysEx protocol (GSX):** the request/response protocol over MIDI SysEx: HELLO, params, presets,
  components, firmware upload. Owner: `protocol/sysex.md`; `gsx.c`, `app_proto.c`.
- **Feedback:** a host lighting LEDs, writing display text, moving rings (F1 plain, F3 DAW scripts).
  Owner: `controller.c` "host feedback", `gm_feedback.h`.
- **Mackie Control (MCU):** the DAW protocol on a second port. Owner: `core/src/mcu.c`.
- **DIN MIDI:** 5-pin MIDI in/out and thru. Owner: `core/src/din.c`.
- **Safety:** unlock before risky requests, status LED, boot and factory-reset combos. Owner:
  `core/src/safety.c`.

### Internals

- **Hub RAM:** one byte array in `vpin.c` that RAM-backed hubs (LED drivers, LED strips) take
  their bytes from; its size is `GM_HUB_RAM`. Owner: `gm_hub_ram`, `gm_hub_ram_alloc` (`vpin.c`, declared in
  `components/hub/gm_hub.h`).
- **Send budget:** the bus bytes the main loop lets hubs, then screens, write per pass (4). Owner:
  `vpin_poll` (returns what the hubs left), `gm_screen_send`.
- **Screen RAM:** the screens' framebuffers and what each panel was sent, `GM_SCREEN_RAM` bytes each.
  Owner: `components/display/screen.c`.
- **Feature gates:** `GM_HUB_TYPES`, `GM_DISPLAY_TYPES`, `GM_DISPLAY_SHOWS` and other generated bits
  that compile unused code out. Owner: `gm_device_config.h`, `#if` in core.
- **Main loop / scan tick:** the two contexts code runs in: `gm_poll` (main loop) and `gm_scan_tick`
  (timer interrupt). Owner: `controller.c`.

## Explain test

A device YAML describes a controller's pins, hubs and controls; the builder checks it and generates the
device config, which the core runs on top of a platform. The scan reads pins, on the MCU or through a
hub's driver, and the controller turns the controls' changes into MIDI messages according to their
params, the active preset and any mode or layer. A host talks to the device over the SysEx protocol to
read and change params and presets or upload firmware, and sends feedback that lights LEDs and writes
text on displays, which draw on hub output pins or on a screen (`display:`).
