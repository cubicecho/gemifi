# gemifi ESP platform

The gemifi HAL (`core/include/gm_hal.h`) and profile store (`config_store.h`) for the **ESP32** (classic, S2, S3), on **ESP-IDF** (C), built with PlatformIO (`framework = espidf`). It's an ESP-IDF component, `gemifi_esp/`, that `firmware/esp/hello` and `firmware/esp/midi-controller` use; with `GM_CORE=all` it also compiles the controller core (`core/src`) and the device config.

| File | What |
|---|---|
| `gemifi_esp/gm_esp.h` | build settings (transport, I2C pins, board id), and the platform calls `main.c` makes |
| `gemifi_esp/hal_esp.c` | time, pins (`gpio_*`), ADC (`adc_oneshot`), I2C (`i2c_master`), uid, reboot, scan timer |
| `gemifi_esp/ledstrip_esp.c` | LED strips (`hal_ledstrip_*`, the `ws2812` hub) by RMT: a frame goes out in the background, then a 300 µs latch |
| `gemifi_esp/midi_esp.c` | MIDI transports (TinyUSB, UART0, NimBLE) and the TX queue |
| `gemifi_esp/fw_esp.c` | firmware upload to the other OTA slot (`hal_fw_ops`, `esp_ota_*`), plus a zlib CRC-32 |
| `gemifi_esp/config_store_esp.c` | the stored settings, one NVS blob a slot |
| `gemifi_esp/CMakeLists.txt`, `idf_component.yml` | the component: its sources, the ESP-IDF components it needs, and `espressif/esp_tinyusb` from the component registry (S2 / S3) |

The firmware projects' shared build files are in `firmware/esp/`: `gemifi_esp.py` (PlatformIO's `pre:` script), `partitions.csv`, and the sdkconfig fragments.

Settings come from the device's `gm_device_config.h` when it's on the include path, and fall back to defaults otherwise (that's how hello builds):
- `GM_MIDI_TRANSPORT`: `GM_MIDI_USB`, `GM_MIDI_SERIAL` or `GM_MIDI_BLE`. It also picks the sdkconfig fragment (below).
- `GM_MIDI_BAUD`: default 115200
- `GM_I2C_SDA` / `GM_I2C_SCL`: GPIO numbers. Without them there is no I2C (the builder asks for `i2c:` when a hub needs it).
- `GM_BOARD_ID`, `GM_DEVICE_NAME`, `GM_USB_VID` / `GM_USB_PID`: hello defaults to board id 0x3F00.

## sdkconfig

`gemifi_esp.py` builds the sdkconfig from `firmware/esp/sdkconfig.defaults` and `sdkconfig.<transport>` (`serial`, `usb` or `ble`, from the device config's `GM_MIDI_TRANSPORT`; without a device config, the chip's default), and ESP-IDF adds `<file>.<target>` for each (`sdkconfig.defaults.esp32s3`, `sdkconfig.ble.esp32`, ...). The result is `<build dir>/<env>/sdkconfig.<transport>`, not a file in the project: ESP-IDF only applies defaults to options a sdkconfig doesn't have yet, so a change to a fragment needs a clean build (`pio run -t clean`, or a new build dir).

- **Every build:** FreeRTOS at 1 kHz, an 8 KB main task, `-O2`, 4 MB flash, `partitions.csv`, logs at warning level.
- **The main loop** is `app_main`'s task. It never blocks, so the task watchdog doesn't watch its core's idle task: on the ESP32 and S3 it runs on core 1 (as Arduino's `loop()` did), the radio and timers on core 0; on the single-core S2 neither idle task is watched.
- **`serial`:** no console and no logs, the bootloader's included: UART0 carries MIDI.
- **`usb`:** TinyUSB with one MIDI interface, no CDC or MSC. The console stays on UART0.
- **`ble`:** NimBLE (Bluetooth controller in BLE-only mode on the ESP32), one connection, no bonding.

## Transports

| `midi.transport` | Chips | What the host sees |
|---|---|---|
| `usb` (default on S2/S3) | S2, S3 | A class-compliant USB MIDI device, `gemifi (<name>)`, serial = MAC, from our own descriptors (`esp_tinyusb` 2.x). Web MIDI works as on the NUC. With `mcu:` (core/README.md "DAW feedback") it has two ports, `<name>` and `<name> DAW` (two cables on one endpoint pair). Received MIDI goes to the core as USB-MIDI packets, so the cable is kept. On a DevKitC-1, use the port marked USB (GPIO19 / 20), not UART. |
| `serial` (default on ESP32) | all | Raw MIDI bytes on UART0, so on the board's USB-UART chip, at 115200 baud. The web updater talks to it over **Web Serial**; a DAW needs a bridge (`ttymidi -s /dev/ttyUSB0 -b 115200` on Linux, Hairless MIDI elsewhere). The boot ROM's text also comes out on this port; it's never inside `F0 … F7`, so the SysEx parser skips it. |
| `ble` | ESP32, S3 | BLE-MIDI 1.0 peripheral (NimBLE), advertised as the device name, the service UUID in the advertisement and the name in the scan response. Untested on hardware. |

Pin numbers are GPIO numbers. ADC readings are the raw 12-bit values (0..4095; the S2's 13 bits shifted down), at 12 dB attenuation.

## Scan timer

An `esp_timer` wakes a FreeRTOS task pinned to the main loop's core, at a priority above it. `gm_scan_tick()` therefore preempts `gm_poll()` as an ISR would, but it runs in task context, so it doesn't need IRAM and isn't restricted the way an ISR is. Missed ticks merge into one. The minimum period is 100 µs.

## Profile storage

COMMIT stores one record a slot, using the same words as the NUC journal (`config_store.h`), as an NVS blob: namespace `gemifi`, key `s<slot>`. Writes are atomic. These are the blobs the Arduino builds' `Preferences` wrote, so an update from one keeps the settings. NVS lives outside the app slots, so OTA updates keep them too. Unknown ids are skipped on load. An esptool **erase** (or ESP Web Tools' "erase device") clears them.

## First flash (USB, the chip's ROM bootloader)

There's no gemifi bootloader on ESP. The first install goes through the ROM bootloader. Hold BOOT and tap RESET if the board doesn't enter it automatically.
- **ESP Web Tools** (browser, Web Serial): every build writes `.pio/build/<env>/webflash/` (or `$PLATFORMIO_BUILD_DIR/<env>/webflash/`). It holds the flash parts and a `manifest.json`: the bootloader, the partition table, the OTA data and the app at 0x10000. Serve that folder and point `<esp-web-install-button manifest="…/manifest.json">` at it.
- **esptool:** `pio run -d firmware/esp/<project> -e <env> -t upload`, or `esptool write-flash` with the offsets from `manifest.json`.

The partition table is `firmware/esp/partitions.csv`: two app slots of 1.9 MB each, plus NVS, at the offsets of the Arduino builds' `min_spiffs.csv`. An OTA image must fit a slot.

## Updates over SysEx (OTA)

HELLO reports role 0 (app) with caps bit 0 (FW upload) and `max_chunk` 256, so the host uploads straight to the app (`protocol/sysex.md` §4.1, §4.2 "A/B devices"):
1. `FW_BEGIN size crc32 board_id fw_version`: `board_id` must equal `GM_BOARD_ID` (otherwise BAD_BOARD); `size` must be non-zero, a multiple of 4 and fit the inactive slot. Then `esp_ota_begin` opens that slot, erasing each sector as the writes reach it, so FW_BEGIN answers at once.
2. `FW_DATA` from offset 0, in order, in multiples of 4 up to 256 bytes. The first byte must be 0xE9 (an app image). A chunk that starts a new 4 KB sector waits for its erase, up to about 50 ms.
3. `FW_END`: the CRC-32 (zlib) of the whole file must match. `esp_ota_end` then checks the image (header, segments, SHA-256) and `esp_ota_set_boot_partition` marks it to boot. Until then the running slot stays the boot slot.
4. `REBOOT(0)` starts it. `REBOOT(1)` does the same, since there's no separate update mode.

The image is the plain `firmware.bin`, with no `.gfw` header. A device on an Arduino build (gemifi before ESP-IDF) takes the ESP-IDF image the same way: same partition table, same NVS.

## Recovery

- A bad upload (CRC, size, image check) leaves the old slot booting. Run FW_BEGIN again.
- If an image boots but is broken (a crash loop, no MIDI), re-flash over USB with the ROM bootloader (above). It can't be bricked from SysEx.
- Safe mode: hold the device's `boot_combo` buttons at reset to start with default settings and ignore the stored profile.

## Test firmware

`firmware/esp/hello`: the transport, HELLO, the Identity Reply, a LOG every second, and BOOT button (GPIO0) → LOG + EVENT + note 60. It also supports REBOOT and FW_* upload. Its board id is 0x3F00. To OTA from hello to a device firmware, build hello with that device's id: `GM_BOARD_ID=0x3F01 pio run -d firmware/esp/hello -e esp32s3`.
