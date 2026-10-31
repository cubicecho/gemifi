# gemifi ESP platform

The gemifi HAL (`core/include/gm_hal.h`) and profile store (`config_store.h`) for **ESP32** (classic, S2, S3) and **ESP8266**, on the Arduino framework (C++). It's a PlatformIO library used by `firmware/esp/hello` and `firmware/esp/midi-controller`.

| File | What |
|---|---|
| `gm_esp.h` | build settings (transport, I2C pins, board id), and the platform calls `main.cpp` makes |
| `hal_esp.cpp` | time, pins, ADC, I2C, uid, reboot, scan timer |
| `midi_esp.cpp` | MIDI transports and the TX queue |
| `fw_esp.cpp` | firmware upload to the other OTA slot (`hal_fw_ops`), plus a zlib CRC-32 |
| `config_store_esp.cpp` | the stored profile: NVS (ESP32) or EEPROM sector (ESP8266) |

Settings come from the device's `gm_device_config.h` when it's on the include path, and fall back to defaults otherwise (that's how hello builds):
- `GM_MIDI_TRANSPORT`: `GM_MIDI_USB`, `GM_MIDI_SERIAL` or `GM_MIDI_BLE`
- `GM_MIDI_BAUD`: default 115200
- `GM_I2C_SDA` / `GM_I2C_SCL`: GPIO numbers. Without them, the board's default `Wire` pins are used.
- `GM_BOARD_ID`, `GM_DEVICE_NAME`, `GM_USB_VID` / `GM_USB_PID`: hello defaults to board id 0x3F00.

## Transports

| `midi.transport` | Chips | What the host sees |
|---|---|---|
| `usb` (default on S2/S3) | S2, S3 | A class-compliant USB MIDI device, `<name> (gemifi)`, serial = MAC. Uses TinyUSB (`ARDUINO_USB_MODE=0`, set by the `esp32s3` env). Web MIDI works as on the NUC. |
| `serial` (default on ESP32, ESP8266) | all | Raw MIDI bytes on UART0, so on the board's USB-UART chip, at 115200 baud. The web updater talks to it over **Web Serial**. Boot ROM/bootloader text also comes out on this port; it's never inside `F0 … F7`, so the SysEx parser skips it. |
| `ble` | ESP32, S3 | BLE-MIDI 1.0 peripheral (the core's built-in BLE library, Bluedroid), advertised as the device name. It adds about 800 KB of flash on the classic ESP32. Untested on hardware. |

Pin numbers are GPIO numbers. On the ESP8266, `A0` is pin 17 and is the only ADC pin. ADC readings are scaled to 0..4095 everywhere (the ESP8266's 10-bit value too).

## Scan timer

- **ESP32:** an `esp_timer` wakes a FreeRTOS task pinned to the `loop()` core, at a priority above `loop()`. `gm_scan_tick()` therefore preempts `gm_poll()` as an ISR would, but it runs in task context, so it doesn't need IRAM and isn't restricted the way an ISR is. Missed ticks merge into one. The minimum period is 100 µs.
- **ESP8266:** `gm_scan_tick()` runs from `loop()` (`gm_esp_loop()`) and while `hal_midi_flush()` waits, paced with `micros()`. A timer1 ISR would need the whole scan path in IRAM, and it would have to stop during every flash write (EEPROM commit, OTA). The trade-off is jitter: up to the longest single `gm_poll()` step, typically well under 1 ms. Flash sector erases during an upload stall it for about 30–50 ms, so velocity matrices are less precise than on the ESP32. If the scan falls more than 4 periods behind, it skips ahead instead of bursting.

## Profile storage

COMMIT stores one record, using the same words as the NUC journal (`'GCFG'`, seq, count, `{id | index<<16, value}…`, crc32).
- **ESP32:** an NVS blob (`Preferences` namespace `gemifi`, key `profile`). Writes are atomic.
- **ESP8266:** the EEPROM flash sector, which holds about 500 values. A power cut during COMMIT loses the profile: the CRC fails and the defaults load.

Both live outside the app slots, so OTA updates keep the profile. Unknown ids are skipped on load. An esptool **erase** (or ESP Web Tools' "erase device") clears it.

## First flash (USB, the chip's ROM bootloader)

There's no gemifi bootloader on ESP. The first install goes through the ROM bootloader. Hold BOOT and tap RESET if the board doesn't enter it automatically.
- **ESP Web Tools** (browser, Web Serial): every build writes `.pio/build/<env>/webflash/` (or `$PLATFORMIO_BUILD_DIR/<env>/webflash/`). It holds the flash parts and a `manifest.json`: bootloader, partitions, `boot_app0` and the app at 0x10000 on ESP32, or `firmware.bin` at 0 on ESP8266. Serve that folder and point `<esp-web-install-button manifest="…/manifest.json">` at it.
- **esptool:** `pio run -d firmware/esp/<project> -e <env> -t upload`, or `esptool.py write_flash` with the offsets from `manifest.json`.

The partition table is `min_spiffs.csv`: two app slots of 1.9 MB each, plus NVS. The ESP8266 uses the board's default layout (d1_mini: 4 MB, about 1 MB sketch). An OTA image must fit the free half.

## Updates over SysEx (OTA)

HELLO reports role 0 (app) with caps bit 0 (FW upload) and `max_chunk` 256, so the host uploads straight to the app (`protocol/sysex.md` §4.1, §4.2 "A/B devices"):
1. `FW_BEGIN size crc32 board_id fw_version`: `board_id` must equal `GM_BOARD_ID` (otherwise BAD_BOARD); `size` must be non-zero and a multiple of 4. Then `Update.begin` runs on the inactive slot.
2. `FW_DATA` from offset 0, in order, in multiples of 4 up to 256 bytes. `Update` erases and writes one 4 KB sector at a time, which takes up to about 50 ms per chunk.
3. `FW_END`: the CRC-32 (zlib) of the whole file must match. `Update.end()` then validates the image and marks it to boot (ESP32: `esp_ota_set_boot_partition`; ESP8266: eboot copies it at the next boot). Until then the running slot stays the boot slot.
4. `REBOOT(0)` starts it. `REBOOT(1)` does the same, since there's no separate update mode.

The image is the plain `firmware.bin`, with no `.gfw` header.

## Recovery

- A bad upload (CRC, size, image check) leaves the old slot booting. Run FW_BEGIN again.
- If an image boots but is broken (a crash loop, no MIDI), re-flash over USB with the ROM bootloader (above). It can't be bricked from SysEx.
- Safe mode: hold the device's `boot_combo` buttons at reset to start with default settings and ignore the stored profile.

## Test firmware

`firmware/esp/hello`: the transport, HELLO, the Identity Reply, a LOG every second, and BOOT button (GPIO0) → LOG + EVENT + note 60. It also supports REBOOT and FW_* upload. Its board id is 0x3F00. To OTA from hello to a device firmware, build hello with that device's id: `PLATFORMIO_BUILD_FLAGS=-DGM_BOARD_ID=0x3F01 pio run -d firmware/esp/hello -e esp32s3`.
