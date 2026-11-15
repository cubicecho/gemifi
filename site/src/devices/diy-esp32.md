---
title: DIY ESP32 builds
description: Build your own MIDI controller on an ESP32, ESP32-S2 or ESP32-S3 with gemifi — what's supported, the example devices, and how to install.
maker: DIY
model: ESP32, ESP32-S2, ESP32-S3
status: in-progress
order: 3
summary: Wire buttons, faders, knobs, encoders or a key matrix to an ESP dev board, describe them in YAML, and gemifi turns it into a MIDI controller.
facts:
  - [MCU, "ESP32, ESP32-S2 or ESP32-S3, on any board PlatformIO knows"]
  - [MIDI, "USB MIDI on the S2 and S3 (native USB); serial MIDI through the board's USB-serial chip on all of them; BLE MIDI on the ESP32 and S3 (checked on a classic ESP32)"]
  - [Controls, "Whatever you wire: buttons, LEDs, pots and faders, encoders, pressure pads, one key matrix (see <a href=\"../../components/\">Components</a>)"]
  - [Expanders, "74HC595, 74HC165, 74HC4051, 74HC4067, MCP23017, PCF8574"]
  - ["USB (gemifi)", "<code>1209:0002</code> “gemifi (&lt;name&gt;)” on the S2 and S3; on the others, the board's own USB-serial chip"]
  - [gemifi platform, "<a href=\"../../platforms/#esp\"><code>esp32</code>, <code>esp32s2</code>, <code>esp32s3</code></a>; board ids <code>0x3F00</code>–<code>0x3FFF</code> are for DIY builds"]
yaml:
  - devices/esp32-example.yaml
  - devices/diy-esp32s3-faders.yaml
  - devices/diy-esp32s3-pads-ws2812.yaml
  - devices/diy-esp32-pot-oled.yaml
  - devices/diy-esp32s3-pot-oled.yaml
photos: No photos of a finished build yet.
---

## Status

The firmware builds for the ESP32, ESP32-S2 and ESP32-S3 from the example
YAML files. One has been checked on real hardware: a classic ESP32 DevKitC
with a pot and an SSD1306 OLED
([`diy-esp32-pot-oled.yaml`]({{ site.blob }}devices/diy-esp32-pot-oled.yaml)),
on 2026-11-12. Its OLED, its pot, BLE MIDI to a Zynthian, updates over
serial from the web updater and the rollback after a failed boot all work.
USB MIDI on the S2 and S3 and settings kept over a restart are not checked
yet. [Tested devices]({{ '/docs/devices/' | url }}) has the list per YAML,
and the checks still to do are in [#25]({{ site.repo }}/issues/25). If you try
one, tell us how it went there.

## Example devices

Start from one of these and change the pins to match your wiring:

| YAML | Board | What's on it |
|---|---|---|
| [`diy-esp32s3-faders.yaml`]({{ site.blob }}devices/diy-esp32s3-faders.yaml) | ESP32-S3-DevKitC-1 | 8 faders on a 74HC4051, 4 buttons with LEDs, 1 encoder, USB MIDI |
| [`diy-esp32s3-pads-ws2812.yaml`]({{ site.blob }}devices/diy-esp32s3-pads-ws2812.yaml) | ESP32-S3-DevKitC-1 | 4x4 pad switches on a switch matrix, 16 WS2812B LEDs under them (one pin, any colour), 4 knobs, USB MIDI |
| [`esp32-example.yaml`]({{ site.blob }}devices/esp32-example.yaml) | ESP32-S3-DevKitC-1 | 4 buttons, 1 LED, 2 pots, 8 faders on a 74HC4051, 1 encoder, USB MIDI |
| [`diy-esp32-pot-oled.yaml`]({{ site.blob }}devices/diy-esp32-pot-oled.yaml) | ESP32 DevKitC (WROOM-32) | 1 pot, a 128 × 64 SSD1306 OLED, serial or BLE MIDI. Checked on hardware |
| [`diy-esp32s3-pot-oled.yaml`]({{ site.blob }}devices/diy-esp32s3-pot-oled.yaml) | ESP32-S3-DevKitC-1 | The same pot and OLED, with USB MIDI |

Every button can send a CC (momentary, toggle or ramp), a note, octave or
transpose shifts, pitch bend or program changes, and all of that can be changed
later from the web page without rebuilding. [Components]({{ '/components/' | url }})
gives an overview of what a YAML can hold; the full format is in
[`core/README.md`]({{ "core/README.md" | repoLink }}).

## How gemifi gets on it

ESP chips have a bootloader in ROM, so there's no gemifi bootloader to install
and no SWD.

1. **Build it.** Create a device from an example in the dashboard, edit the
   pins, and build. The first ESP build downloads about 6 GB of toolchains.
2. **First install** over USB with the web updater's “Install firmware (first flash)” card,
   which uses Web Serial (Chrome, Edge or Opera on a desktop). If the board
   doesn't enter its bootloader by itself, hold BOOT and tap RESET.
3. **Later updates** go to the board's spare firmware slot over MIDI SysEx, from
   the same page. The running firmware keeps working until the new one has
   been checked, so a failed upload can't brick it.

If an update boots but doesn't work, do a first install again. Holding the
device's `boot_combo` buttons at reset starts with default settings.

## Back to stock

A dev board has no stock firmware worth keeping: flash anything else on it with
esptool or the browser. Erasing the flash also clears gemifi's saved settings.

## Risks

- **Wiring is up to you.** ESP pins are 3.3 V; check your faders, LEDs and
  expanders against that. The validator warns about strapping and flash pins
  and other pins best left alone, but it can't see your circuit.
- **Shared control lines** between expander chips are allowed only when you
  mark them, and making that work electrically is up to you.
