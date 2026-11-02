# gemifi

An "ESPHome for MIDI controllers": open firmware for existing MIDI controllers (and new builds), with everything after the first install done over USB from a web page. That covers firmware updates, debug logs and, later, config such as what each button does.

First target: **M-Audio Keystation Mini 32 MK3** (Nuvoton NUC120). Also: custom builds on **ESP32 / ESP32-S2 / S3 / ESP8266**, described ESPHome-style in a device YAML (pins, buttons, LEDs, pots, encoders, key matrix, and expanders like 74HC595/165/4051/4067, MCP23017, PCF8574). Later: M-Audio Code 61 (STM32F103) and the Behringer FCB1010.

## How it fits together

```
┌──────────────────────────────┐
│ config  (data flash)          │  edited from the web page over SysEx
├──────────────────────────────┤
│ app firmware (APROM)          │  replaced over USB, carries a .gfw header
├──────────────────────────────┤
│ bootloader (LDROM, 4 KB)      │  flashed once over SWD; never writable from USB
└──────────────────────────────┘
```

Everything speaks one protocol, **MIDI SysEx** (over USB, serial or BLE), defined in [`protocol/sysex.md`](protocol/sysex.md). On ESP chips there is no gemifi bootloader: the first flash uses the chip's ROM bootloader (Web Serial / esptool), and updates go to the OTA slot through the app itself.

```
devices/<device>.yaml ──tools/builder──▶ generated tables ─┐
core/ (protocol, config, controller runtime)               ├─▶ firmware image ──▶ web/updater
platform/<chip family>/ (HAL: pins, ADC, MIDI, storage)    ┘
```

## Layout

| Folder | What's in it |
|---|---|
| [`core/`](core/) | Chip-independent C: protocol codec, config/schema, the MIDI controller runtime, virtual pins and expanders. **Device YAML format: [`core/README.md`](core/README.md)** |
| [`devices/`](devices/) | Device descriptions (presets): Keystation, ESP examples |
| [`protocol/`](protocol/) | The SysEx protocol and `.gfw` image format: the contract between all the parts |
| [`bootloader/`](bootloader/) | Bootloaders, one folder per chip family (`nuvoton-nuc1xx/`) |
| [`firmware/`](firmware/) | App firmware and test apps, per chip family |
| [`platform/`](platform/) | Per chip family: startup, clocks, USB, flash, the HAL for the core (`nuvoton-nuc1xx/`, `esp/`) |
| [`web/updater/`](web/updater/) | Browser flasher, log viewer and config editor (Web MIDI) |
| [`tools/`](tools/) | SWD scripts (dump, flash, restore), host tools (`gemifi.py`, `mkimage.py`), the device builder and dashboard (`builder/`), USB probes. **Start with [`tools/README.md`](tools/README.md).** |
| [`research/`](research/) | Notes and analysis: `core/` (design, framework), `chips/` (per chip family), `devices/` (per device) |

## Quick start: the dashboard

Define a device, edit its YAML, build it, and flash it from the browser, ESPHome-dashboard style:

```sh
docker compose up -d --build     # http://localhost:6055/  (or: tools/builder/gemifi_build.py dashboard)
```

Your devices live in `./config`. Flashing over Web MIDI / Web Serial needs `localhost` or https (`GEMIFI_TLS=self-signed`); see [`tools/builder/README.md`](tools/builder/README.md) "Docker".

On a docker host without the source, run the published image `ghcr.io/cubicecho/gemifi` with [`docker-compose.ghcr.yml`](docker-compose.ghcr.yml); see "Deploy on a docker host" there.

## Status (2026-11-02)
Early and experimental: expect the YAML format and the protocol to change.

- **Keystation Mini 32 MK3: works.** The gemifi bootloader is in LDROM (installed once over SWD) and the generic `midi-controller` app is installed over USB. Checked on hardware on 2026-10-31: all 32 keys with velocity, all 7 buttons, the volume knob, all LEDs, settings from the web page, app updates over USB (about 4 s) and app ↔ bootloader reboots (milestones M1–M4 in [`tools/README.md`](tools/README.md) §4a).
- **Merged but not yet checked on hardware:** the bootloader's boot-loop guard and stored serial, the resumable SWD verify, DAW feedback (host-driven LEDs, a second "DAW" port, the Mackie Control framework), and the USB product name "gemifi (<board>)".
- **ESP32 / S2 / S3 / ESP8266:** build and pass host tests; not yet checked on hardware.
- **Code 49/61, FCB1010:** researched or planned (`research/devices/`).

Stock firmware dumps and vendor files are kept locally and are **not** committed (see `.gitignore`).
