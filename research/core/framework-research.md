# An ESPHome-style framework for MIDI controllers: research

2026-10-30. This combines four research passes: Code-series firmware, ESPHome internals, prior art, and ESP32 hardware. It builds on the Code 49 firmware analysis in [m-audio-code61](../devices/m-audio-code61/README.md). Facts the agents couldn't verify are marked (unverified).

## TL;DR

- **The idea fills a gap.** Nothing gives you one declarative file that describes both a controller's hardware and its behaviour, with rich feel tuning (velocity curves, adaptive deadband, acceleration curves) and a generic web editor. OpenDeck comes closest.
- **Don't build on ESPHome.** It has no STM32 support, its sensor model is built for slow telemetry (the ADC defaults to a 60 s update interval), and every tweak needs a rebuild. Copy its *architecture* instead.
- **Use a hybrid model:**
  - **Hardware** (pins, matrix, mux, transports) is fixed at build time and generated from YAML, as ESPHome does.
  - **Behaviour and feel** (mappings, curves, deadbands, acceleration) is set at runtime: stored in flash, edited live over SysEx.
  - The firmware **describes itself**: it can send its own schema. The fcb1010 web editor becomes a generic editor that renders forms from that schema.
- **Code series:** the update-block checksum is now solved (it's the same scheme as the Arturia KeyStep). We can build valid update files, so there are two ways to install firmware: the stock M-Audio updater, and SWD.
- **Install and update from the browser** (§7): the same page that edits the profile also installs firmware.
  - ESP32: Web Serial for the first install, then SysEx updates over Web MIDI.
  - Code series: SysEx through M-Audio's own bootloader.
  - On the ESP32, one generic image can handle any wiring, set up at runtime, so DIY boards need no compile step.
- **ESP32 test target:** an ESP32-S3. It is the only family with USB MIDI, BLE MIDI and Wi-Fi together, plus a hardware encoder counter (PCNT) and a better-calibrated ADC.

---

## 1. Prior art and what's missing

| Project | Config model | Targets | Feel tuning | License | Take |
|---|---|---|---|---|---|
| **OpenDeck** (shanteacontrols) | Hardware = Zephyr devicetree overlay per board; behaviour = runtime SysEx (block/section/index, 14-bit values) + Web MIDI configurator | RP2040/2350, ESP32-S3, STM32F4/F7/H5/H7, nRF52, Teensy 4, SAMD51. **No STM32F1** | Encoder modes + Slow/Med/Fast accel, analog end-trim, FSR velocity. No curves, deadband or keybed velocity | Apache-2.0, active (last commit Sep 2026) | Closest match. Borrow the split and the SysEx + Web MIDI pattern |
| MIDIbox NG / MIOS32 | Runtime `.NGC` text files on SD | Own LPC17/STM32F4 cores (F103 for older MIOS32) | Good, incl. velocity keybeds | Non-commercial only | Ideas for event, bank and keybed semantics. Don't reuse the code |
| Control Surface (tttapa) | Compile-time C++ | AVR, ESP32, Teensy, Pico… | EMA + hysteresis filters, mapping functions | GPL-3.0 | Reimplement its filter ideas; don't copy the code |
| Intech Grid | Runtime Lua per control, Grid Editor | ESP32-S3 + RP2040 | Script-defined | GPL-3.0 | Lua as an escape hatch |
| Electra One | JSON preset (published schema, versioned) + Lua | Own hardware | Per preset | Proprietary | Versioned JSON schema as the exchange format |
| VIA / Vial | Board-definition JSON + raw-HID protocol + web app | QMK keyboards | n/a | GPL | **The best example for a generic editor**: one editor, many devices, each describing itself |
| ESPHome | YAML → validated → generated C++ | ESP32/8266, RP2040, LibreTiny, nRF52, host | Sensor `filters:` chain | MIT/GPL | Architecture to copy (see §2) |

Only one ESPHome MIDI component exists, `muxa/esphome-midi`, and it handles DIN MIDI input only. No project calls itself "ESPHome for MIDI".

**What's missing (what this project would add):**
1. **One `controller.yaml`** for hardware *and* behaviour, which you can diff and version. OpenDeck splits these into a developer-only devicetree and opaque SysEx indexes.
2. **Feel tuning as first-class config:** two-contact keybed velocity curves, custom curve tables, adaptive deadband and smoothing, and continuous acceleration curves.
3. **Replacement firmware for commercial hardware.** No open project targets stock controllers like the Code series.
4. **Editors that build their UI from the device's own schema.** VIA does this for keyboards; nothing does it for MIDI controllers.

## 2. What to take from ESPHome

How it works:
1. **YAML** is loaded with `!secret`, `!include`, `packages:` and `substitutions:`.
2. Each component's **`CONFIG_SCHEMA`** (voluptuous) validates its part of the config and fills in defaults. `DEPENDENCIES` and `AUTO_LOAD` pull in other components.
3. Each component's **`to_code()`** emits C++ through a `MockObj` expression builder (`cg.add(var.set_pin(...))`) into `setup()` in `main.cpp`. `await cg.get_variable(id)` handles ordering between components.
4. At **runtime**, `Component` objects have `setup()`/`loop()`, setup runs in priority order (BUS > IO > HARDWARE > DATA…), and one cooperative main loop drives everything.
5. The generated project is **built** with ESP-IDF/CMake (the default since 2026.7) or PlatformIO, then **flashed** over serial, Web Serial or OTA.

**Copy:**
- schema-first validation that holds the defaults
- generated code that only calls setters on plain C++ classes, which gives static allocation (matters with 20-64 KB of RAM)
- priority-ordered setup
- the **filter chain** (`raw → filter → filter → output`, each stage a small class)

**Avoid:**
- structural tweaks needing a rebuild (1-5 min a time)
- C++ lambdas inside YAML strings (errors only show up at compile time)
- per-entity overhead. A 61-key matrix must be a single component that runs inside the scan loop, not 61 sensors.

## 3. Proposed architecture

```
controller.yaml ──► gemifi compiler (Python) ──┬─► generated board code (pins, matrix, mux, transports)
                                               │      + generic runtime core ──► firmware image
                                               │                                  (.bin / stock-updater .syx / UF2)
                                               ├─► default profile blob (feel + mappings)
                                               └─► schema.json (controls, params, ranges, layout)
                                                           │
                         web editor (fcb1010 grown up) ◄───┘  via SysEx: identify → get schema → get/set params → save
```

### Build time vs runtime

| Build time (hardware) | Runtime (profile; stored in flash, editable live) |
|---|---|
| MCU/board, pins, key matrix wiring, mux select lines, ADC channels | CC/note/NRPN per control, channel, ranges, invert |
| Transports compiled in (USB, DIN, BLE, RTP-MIDI) | Velocity curve tables (keys, pads), key time window and offset |
| Number and type of controls, LEDs, display | Pad threshold, full scale, re-trigger rules |
| Filter chain *structure* per input type | Filter *parameters*: EMA alpha, deadband, hysteresis |
| | Encoder detent size, acceleration curve, reversal handling, relative mode |
| | Presets/banks |

### The feel pipeline as config

Each "mushy" cause found in the Code 49 firmware becomes a knob in the profile:

| Code 49 stock behaviour | Profile parameter |
|---|---|
| Pads: 127 needs ~95% of full scale; fires on the 3rd sample | `pad.full_scale`, `pad.threshold`, `pad.fire_after_samples`, `pad.curve` |
| Keys: everything ≤6 ms → 127; curve linear in time | `keys.min_ms`, `keys.max_ms`, `keys.curve` (log-of-time default) |
| Faders: 160-count deadband when idle, 4-sample block average | `analog.filter: adaptive_ema` + `hysteresis_lsb: 0.5` + `sleep_after_ms` |
| Encoders: acceleration on by default, reversals dropped | `encoder.accel: {k, v0, p}`, `encoder.reverse: instant` |
| Wheels: ±10-count hysteresis | `wheel.hysteresis`, `wheel.resolution: 14bit` |

Filter recommendations from the hardware research:
- oversample, then take a 1 ms decimated average (no large block averages)
- an adaptive EMA (ResponsiveAnalogRead / 1€-filter style): heavy smoothing at rest, light while moving
- hysteresis applied at the *output* resolution (~0.5 LSB)
- a threshold that widens only after the control has been still for N ms

Encoder acceleration: `step = 1 + k·max(0, v − v0)^p` on smoothed detent speed, so slow turns stay exactly 1:1, and reset instantly on reversal.

### Sketch of a config file

```yaml
gemifi:
  name: knobbox
board: esp32-s3-devkitc-1
transports:
  usb_midi: { cables: 1 }
  ble_midi: {}
inputs:
  - encoder: { id: enc1, pins: [GPIO4, GPIO5], detent: 4 }
  - pot: { id: pot1, adc: ADC1_CH0, filter: adaptive_ema }
profile:                       # default; editable at runtime
  enc1: { cc: 20, mode: relative_2s, accel: { k: 0.6, v0: 8, p: 1.5 } }
  pot1: { cc: 21, hysteresis_lsb: 0.5, curve: linear }
```

### Device protocol (what the editor needs)

- **Identify:** Universal Identity Request (`F0 7E 7F 06 01 F7`). Use manufacturer ID `7D` (non-commercial) while prototyping.
- **Commands:**
  - `GET_SCHEMA` returns a compressed JSON schema split into SysEx chunks. On small flash, send only a hash and let the editor fetch the matching schema from the firmware bundle.
  - `GET` / `SET` a parameter (numeric ID, from the schema), `GET` / `SET` a whole profile blob, and `SAVE`.
  - Value encoding uses OpenDeck-style 14-bit values in 7-bit pairs.
- **`MONITOR`** streams raw, filtered and output values for a control. This is the feature that makes feel tuning work: you see the ADC noise and the deadband effect live while dragging a slider.
- **Later:** export the control map as MIDI-CI Property Exchange (`CtrlMapList`) so DAWs can see it. The MIDI-CI spec is too young to be the main protocol, and the agents found no open embedded implementation.

### fcb1010 → generic editor

Most of the repo is already reusable:
- **Device-neutral:** `src/lib/midi/*` (decoder, names, ports), the monitor, the MIDI value pickers, and the UI kit.
- **FCB-specific:** `src/lib/fcb/` and `components/fcb/`.

Steps:
1. Define a `Device` interface: `detect()`, `readAll()`, `writeAll()`, `get/set(param)`, `schema()`. Move the FCB code behind it as the first "driver".
2. Add a **schema driver** that builds Presets and Setup pages from the device's `schema.json` (control groups, a layout grid like the FCB pedal board, enums, curve editors).
3. Optionally, describe the FCB1010 memory image in a static schema too (byte offsets in place of SysEx param IDs). Then every device, old or new, goes through one renderer, as VIA does.
4. Keep the Web MIDI limits in mind:
   - Chrome now asks permission for all MIDI access, not just SysEx.
   - Safari has no Web MIDI.
   - Firefox needs a site-permission add-on.

   A WebSerial or desktop-wrapper fallback may come later.

## 4. Target: M-Audio Code series

What we know now:
- **MCU:** STM32F103 high-density, with no encryption. "High-density" means 256-512 KB flash and 48-64 KB RAM. The ~89 KB is the size of the app image, not the chip. Read the real size from the flash-size register (0x1FFFF7E0) over SWD.
- **Updater:** almost certainly SysEx over class-compliant USB MIDI. Evidence: the 1.2.1 release notes mention a Mac SysEx transfer fix.
- **Update file:** the block checksum is **solved**. It is the negated 16-bit sum of the bytes from offset 2, stored little-endian. The file ends with page-erase records and a metadata block at 0x0801B000. See the code61 README.
- **Newer firmware:** an updater 1.3.0 (firmware 1.0.4.4) existed; the link is dead, but archive.org may have it. Worth comparing.
- **Hardware details:** no public teardowns or schematics, and no community reverse-engineering of Code, Oxygen or Keystation. You'll have to open the unit.
- **Precedents:**
  - *Novation Launchpad Pro:* also an STM32F103. It has a protected bootloader and SysEx updates, so it "can't brick".
  - *Arturia KeyStep:* also an STM32F103. Patched firmware was delivered through the stock updater using this same checksum.
- **Bootloader entry:** the Hammer 88 uses a key-hold combo to enter its bootloader, so the Code may have one too (unverified).

### Firmware stack for the F103

| Option | License | Notes |
|---|---|---|
| **TinyUSB + ST LL / libopencm3** | MIT / LGPL | Small (USB MIDI under 20 KB), supports F1 USB device mode (`stm32_fsdev`). **Recommended** |
| Zephyr (`usbd_midi2`, 4.1+) | Apache-2.0 | Lets us share OpenDeck's base, but heavier; F1 + MIDI2 class untested |
| MIOS32 | Non-commercial | Reference only |
| STM32duino USBComposite | MIT-ish | Maintenance mode |

### Flashing and safety
1. **Before anything else:** use SWD read-only to probe for read-out protection (RDP) and dump the full flash. Also dump the 9 KB bootloader, which we don't have.
   - F1 chips only have RDP levels 0 and 1.
   - **Never** remove RDP if it is set: that mass-erases the chip, M-Audio bootloader included.
2. Link our firmware at **0x08002400** and point the vector table (VTOR) there, so the stock bootloader stays as the recovery path.
3. **Install methods:**
   - **A:** wrap the image in M-Audio's update format (possible now that the checksum is known) and send it with the stock updater, or our own Web MIDI tool once the SysEx framing is captured.
   - **B:** write it over SWD.

## 5. Target: ESP32

| Chip | USB MIDI | BLE MIDI | Wi-Fi | PCNT |
|---|---|---|---|---|
| **S3** | ✓ | ✓ | ✓ | ✓ (4 units, unverified) |
| S2 | ✓ | ✗ | ✓ | ✓ |
| P4 | ✓ (incl. HS) | ✗ | ✗ | ✓ (unverified) |
| C3 | ✗ | ✓ | ✓ | **✗** |
| C6 / H2 | ✗ | ✓ | C6 only | ✓ |
| classic | ✗ | ✓ | ✓ | ✓ (8) |

**Boards:**
- Start on an **ESP32-S3-DevKitC-1 (N8R8)**: use the OTG port for MIDI and the UART port for flashing and logs.
- For a small controller: ESP32-S3-Zero or Lolin S3 Mini.
- Later, a C3/C6 SuperMini as a "BLE + DIN only" target.

**Software stack:** ESP-IDF + `esp_tinyusb` (TinyUSB MIDI), NimBLE for BLE MIDI, and a UART at 31250 baud for DIN.

**Encoders:** use PCNT with its hardware glitch filter, which handles bounce at no CPU cost. Fall back to a GPIO state-machine decoder on the C3 or past four encoders.

**Pots:**
- Put them on ADC1, never ADC2 (it conflicts with Wi-Fi).
- Use continuous (DMA) ADC mode, oversampled and decimated to 1 ms.
- Expect about 9-10 effective bits: fine for 7-bit CC. True 14-bit needs an external ADC (MCP3208).
- For many channels, use CD4051 or 74HC4067 multiplexers.

**Tasks:**
- a 1 kHz scan task pinned to core 1
- a queue feeding a MIDI-out task that sends to each transport
- radio and TinyUSB on core 0
- sending only on change, at most once per ms per control

**Latency:** USB ≈1-2 ms; BLE ≈10-20 ms (7.5 ms minimum connection interval); DIN ≈0.32 ms per message.

**Quirks:**
- macOS caches USB descriptors: bump the PID or bcdDevice when the descriptors change.
- Arduino builds must use the USB-OTG (TinyUSB) mode, or the board only enumerates as serial/JTAG.

## 6. Build our own or fork OpenDeck?

**Recommendation: write a small core of our own (TinyUSB plus a thin per-target HAL) and copy OpenDeck's ideas rather than its code.**

Reasons:
- **Self-describing schema:** the generic editor needs it, but OpenDeck's numeric block/section/index protocol isn't self-describing. That would have to be redesigned.
- **Code-specific work:** OpenDeck has no F1 target and no two-contact keybed. Both are core work for the Code anyway.
- **Feel tuning:** this is the main differentiator, and it sits deep in the input pipeline.

**Counterpoint:** OpenDeck already runs on ESP32-S3 with a working Web MIDI configurator. Spend half a day flashing it on the S3 to experience its UX before deciding; if it fits better than expected, extend it rather than starting from scratch.

## 7. Install, set up and tune from the browser

The goal: plug the device into a PC over USB, open the web editor, install a base image, set up the hardware, then tune. No toolchain on the user's machine. ESPHome does the install part with ESP Web Tools / esptool-js over Web Serial; we can do the same, and for most cases do it over Web MIDI instead.

### The flow

1. **Connect and identify.** The editor works out what's plugged in:
   - **Our firmware:** it answers the Universal Identity Request and reports its model, version and schema.
   - **Stock M-Audio firmware:** recognised by M-Audio's Identity Reply and port name.
   - **A blank or stock ESP32:** found over Web Serial by the ROM bootloader (esptool-js reads the chip type).
2. **Back up.** Before writing anything, save whatever we can from the device to a file:
   - Code: the "Sending and Restoring SysEx Memory" dump from the manual (user settings, possibly the per-unit calibration).
   - ESP32: the existing flash, read with esptool-js.
3. **Install the base image** (see "Transports" below).
4. **Hardware setup** (generic boards only): choose which pins are encoders, pots, buttons, muxes or LEDs. The device checks the result (pin conflicts, ADC1-only pots on ESP32) and reboots. Fixed devices like the Code skip this step: their layout comes with the image.
5. **Tune:** feel, curves and mappings, with the live monitor.
6. **Later updates** go through the same page. The device's settings survive because they live in their own flash area, separate from the app.

### Two kinds of image

| | Fixed device (Code 25/49/61) | Generic board (DIY ESP32-S3) |
|---|---|---|
| Image | Prebuilt, one per model | Prebuilt, one per chip family |
| Hardware layout | Built in, from a board file we derive from the stock firmware (mux slot → pad map at 0x08014C08, key scan map at 0x08014C18, ADC channels) | **Set at runtime** and stored in NVS. The ESP32 GPIO matrix can route almost any function to any pin, so one image can drive any wiring (this is how WLED works) |
| Default profile | Imported from stock: the factory defaults at 0x080155C0, plus that unit's own calibration from its EEPROM/SysEx dump where we can read it | Per-input-type defaults |
| Compile step | No (we build it once and publish it) | **No** |

That changes the architecture in §3: **on the ESP32, the hardware layer can be runtime too.** The YAML → codegen path stays for:
- tight targets like the F103 (20-64 KB of RAM);
- unusual hardware the generic image can't drive;
- people who want a custom build.

YAML also becomes the **export/import format** for a device's whole config, so a setup can be shared and kept under version control.

### Transports, per target

**ESP32-S3**
- **First install and recovery:** Web Serial + esptool-js, as ESP Web Tools does. The S3 ROM bootloader shows up as a USB serial device on the same port the OTG MIDI uses, so no extra USB-UART chip is needed (unverified on our boards; check in the spike).
- **Updates:** a SysEx command writes the new image into the inactive OTA slot (`esp_ota_*`). The device checks it, switches slots and reboots, and rolls back if the new image fails to start. This is pure Web MIDI, with no serial port, drivers or button presses.
- **Fallback:** a SysEx "reboot into ROM download mode" command (`RTC_CNTL_FORCE_DOWNLOAD_BOOT` + reset). The editor then switches to Web Serial.

**Code series**
- **Install:** the updater speaks plain Web-MIDI-compatible SysEx, and the protocol is now worked out ([update-protocol.md](code61%20research/analysis/update-protocol.md)):
  1. Send the start message `F0 5A 57 6E 28 3C 4E 45 F7`.
  2. Send each record as 11-byte chunks of ASCII hex inside `F0…F7`.
  3. Wait for `F0 45 F7` after each record. `F0 15 F7` means it was rejected.

  Settings live at 0x0801B400 and survive updates.

  **Still open:** how the keyboard enters update mode. The UI just says "turn the device off and on", so it needs a hardware test.
- **Our firmware:** implements a "reboot to bootloader" SysEx command, so later updates are one click.
- **Recovery:** the M-Audio bootloader is never written, so a failed update can always be retried. SWD remains the last resort.

**Later**
- RP2040: UF2 or `picotool` over WebUSB.
- STM32F4: ROM DFU, which WebDFU can drive.

### "Restore stock firmware"

A restore button is easy technically, since the update format is now fully known. But we shouldn't host M-Audio's firmware. Instead, have the user supply the official updater `.exe` and pull the image out in the browser (a TypeScript port of `extract_firmware.py`: Burn → MSI → CAB → hex). The same path can import the stock defaults and tables into the board file.

### Browser limits

- Web MIDI and Web Serial are both **Chromium-only**: Chrome, Edge, Brave and Opera on desktop. Firefox needs a permission add-on; Safari has neither.
- The page must be served over HTTPS. GitHub Pages, which fcb1010 already deploys to, is fine.
- Chrome asks for MIDI permission, and separately for each serial port.
- On Windows, only one app at a time can open a MIDI port, so the DAW has to let go of the device while you update or tune.
- Images are published as GitHub release assets with a manifest (ESP Web Tools manifest format for the ESP32), including a checksum the editor verifies before sending.

### Target: Behringer FCB1010

**Hardware**
- Philips P80C32 (8051-family) running from a socketed one-time-programmable 27C256/27C512 EPROM.
- 32 KB RAM.
- A 24C16 2 KB EEPROM for settings.
- An 8-bit ADC for the optical expression pedals.
- 74HC273-latched 7-segment display and switch/LED scan.
- DIN MIDI only (H11L1). **No USB.**

Source: practicalusage.com teardown, Wikibooks.

**Firmware:**
- **Stock, UnO and UnO2 are all EPROM chip swaps**, with no update mechanism of any kind.
- UnO2 is licence-locked and has no USB.
- Wino2, from the UnO author, is a Wi-Fi module that plugs into the internal 4-pin MIDI link between the main board and the MIDI board. It shows an add-on there works without soldering.

**What fits gemifi:**

| Level | How | Web install? | Effort |
|---|---|---|---|
| **Runtime config (today)** | The existing UnO dump protocol over DIN MIDI (fcb1010 editor). Gemifi supplies a **schema on the host side** for it, since the device can't describe itself | n/a | Low: an adapter in the generic editor |
| Bridge module | An ESP32-S3 on the internal 4-pin MIDI link, like Wino2. It gives USB-MIDI and Wi-Fi, describes itself, translates the schema into UnO dumps, and passes MIDI through. Installed and updated from the browser (the module's own firmware) | Yes, for the module | Medium. No soldering; feel is still limited to what UnO does |
| New 8051 firmware | SDCC, starting from `jspyve/FCB1010-lib` (public domain). Burned to a flash drop-in EPROM, or a flash 8051 with a serial bootloader | No (chip swap, or a serial bootloader plus a reset switch) | High. Little upside |
| **Brain swap** | An ESP32-S3 board on the original 2.00 mm switch/LED/pedal connectors, keeping the DIN board. Precedents: practicalusage PU-2 (Arduino Mega), TEE1010 (Teensy 4.1). Needs care with 3.3 V (switch pull-ups, LED drive, pedal scaling, MIDI-out resistors) | **Yes**, the full gemifi experience | Medium-high. A good "fixed device" board file |

**Recommendation:**
- **Now:** treat the FCB1010 as a runtime-config-only device in the generic editor. Its hand-written schema maps onto the dump bytes; this is the static-schema driver from §3.
- **Later:** the brain swap is the route to firmware updates and feel tuning in the browser. The chosen design is an ESP32-S3 with USB-C only and the mains section removed. Its design notes, pinouts, power budget and measurement list are in [fcb1010-brainswap.md](../devices/behringer-fcb1010/fcb1010-brainswap.md).

## 8. Suggested next steps

0. ~~**Code, static only:** recover the updater's SysEx framing.~~ Done: `update-protocol.md`. How the keyboard enters update mode is still open (hardware test).
1. ~~**Code, no risk:** an update rebuilder plus a round-trip test.~~ Done: `scripts/update_protocol.py`, byte-identical on the stock file.
2. **Code hardware:**
   - Open the unit and photograph the board: chip marking, SWD pads, BOOT0, EEPROM, pad sensor type.
   - Probe RDP read-only with an ST-Link, then dump the full flash.
3. **Code quick win (optional):** apply the byte patches from `mushy-feel.md` through the update path. This checks the install path and the feel theory before writing any new firmware.
   - Risk: the bootloader may also verify the app image, and we don't know how yet.
4. **ESP32-S3 spike:**
   - Two encoders plus four pots and USB MIDI.
   - The runtime profile over SysEx with `MONITOR`.
   - Tune the feel live from a stub page in the editor.
5. **Generalise the editor:** the `Device` interface and schema driver in fcb1010.
6. **Compiler:** YAML → schema validation → generated board code, schema.json and default profile.
7. **Code port:** F1 HAL, two-contact key matrix, pad/fader mux, linked at 0x08002400, installed via the stock update format.

## Sources

- **OpenDeck:**
  - https://github.com/shanteacontrols/OpenDeck
  - wiki: [SysEx-Configuration](https://github.com/shanteacontrols/OpenDeck/wiki/SysEx-Configuration), [Creating-custom-board-variant](https://github.com/shanteacontrols/OpenDeck/wiki/Creating-custom-board-variant), [Supported-microcontrollers](https://github.com/shanteacontrols/OpenDeck/wiki/Supported-microcontrollers)
- **ESPHome:**
  - https://developers.esphome.io/architecture/overview/
  - https://github.com/esphome/esphome/blob/dev/esphome/codegen.py
  - https://github.com/esphome/esphome/blob/dev/esphome/core/component.h
  - https://esphome.io/components/sensor/
  - https://esphome.io/blog/2026/07/15/esphome-2026-7/
- **ESPHome MIDI:** https://github.com/muxa/esphome-midi
- **KeyStep firmware patching:** https://dsgruss.com/notes/2020/10/02/keystep1.html
- **Launchpad Pro open firmware:** https://github.com/dvhdr/launchpad-pro
- **TinyUSB:** https://github.com/hathach/tinyusb
- **Zephyr USB MIDI 2:** https://docs.zephyrproject.org/latest/doxygen/html/group__usbd__midi2.html
- **STM32 RDP:** https://wiki.st.com/stm32mcu/wiki/Security:RDP
- **ST bootloaders:** AN2606
- **Control Surface:** https://github.com/tttapa/Control-Surface
- **Intech Grid:** https://github.com/intechstudio/grid-fw
- **Electra One preset format:** https://docs.electra.one/developers/luaext.html
- **MIDI-CI:**
  - https://midi.org/midi-ci-specification
  - https://midi.org/midi-ci-property-exchange-controller-resources
- **Web MIDI permission:** https://developer.chrome.com/blog/web-midi-permission-prompt
- **ESP32:**
  - PCNT: https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-reference/peripherals/pcnt.html
  - ADC calibration: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/adc/adc_calibration.html
  - esp_tinyusb: https://components.espressif.com/components/espressif/esp_tinyusb
  - NimBLE-Arduino: https://github.com/h2zero/nimble-arduino
