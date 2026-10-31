# midi-controller (ESP)

The generic gemifi controller (`core/`) for ESP32 / ESP32-S2 / ESP32-S3 / ESP8266. It's built from a device's generated tables. Platform notes (transports, first flash, OTA, recovery) are in [`platform/esp/README.md`](../../../platform/esp/README.md).

## Build

```sh
pio run -d firmware/esp/midi-controller -e esp32s3                       # devices/esp32-example.yaml
GM_DEVICE=devices/my-device.yaml pio run -d firmware/esp/midi-controller -e esp32s3
```

The device config is generated from the YAML at every build (into `<build dir>/<env>/gm_device_config/`; files are only rewritten when they change, so an unchanged YAML rebuilds nothing). It needs the system `python3` with PyYAML, as `tools/builder` does.

## Build interface (used by tools/builder)

```sh
GM_DEVICE_DIR=<dir with gm_device_config.h/.c> \
PLATFORMIO_BUILD_DIR=<out>/pio \
GM_PIO_EXTRA=<optional ini> \
pio run -d firmware/esp/midi-controller -e <esp32|esp32s2|esp32s3|esp8266>
```

- `GM_DEVICE_DIR`: an already generated config; an absolute path, or a path relative to the repo root. `firmware/esp/gemifi_esp.py` adds it to the include path and compiles its `.c` files together with `core/src/*.c`. Without it, the config is generated from `GM_DEVICE` (a YAML path, same rules), which defaults to `devices/esp32-example.yaml` for `esp32s2`/`esp32s3` and `devices/esp8266-example.yaml` for `esp8266`. `esp32` has no default and needs a device.
- Device settings come from `gm_device_config.h`: `GM_MIDI_TRANSPORT`, `GM_I2C_SDA`/`GM_I2C_SCL`, `GM_BOARD_ID`, `GM_DEVICE_NAME`, `GM_USB_VID`/`GM_USB_PID`, and optionally `GM_FW_VERSION` (from the YAML `version:`, default 0.1.0) and `GM_MIDI_BAUD`.
- **Board:** `pio run` has no `--project-option`. To use a board other than the env's default (`esp32dev`, `esp32-s2-saola-1`, `esp32-s3-devkitc-1`, `d1_mini`), set `GM_PIO_EXTRA` to an ini file that overrides the env. The env's `board_build.*` settings still apply.
  ```ini
  [env:esp32s3]
  board = lolin_s3
  ```
- Output: `<build dir>/<env>/firmware.bin` (the OTA image), plus `bootloader.bin`, `partitions.bin`, and `webflash/` (the parts and `manifest.json` for ESP Web Tools; its `version` is the YAML `version:`, or `dev`).

`PLATFORMIO_BUILD_FLAGS` adds compiler flags (e.g. `-DGM_FW_VERSION=0x00010200`).

## Examples

- `devices/esp32-example.yaml`: ESP32-S3 with USB MIDI, 4 buttons, 1 LED, 2 pots, 1 encoder, and 8 faders on a 74HC4051.
- `devices/esp8266-example.yaml`: D1 mini with serial MIDI, 2 buttons, 1 LED, and a pot on A0.

They are the build's defaults (see Build).

## Schema

The params are the core's (`core/src/settings.c`): `midi.*`, `button.*`, `analog.cc`, `encoder.*` and so on, sized from the device's component counts.
