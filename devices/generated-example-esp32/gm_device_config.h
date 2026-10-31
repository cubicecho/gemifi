/* gm_device_config: do not edit; generated from devices/esp32-example.yaml
 * by tools/builder/gemifi_build.py (format: core/README.md). */

#ifndef GM_DEVICE_CONFIG_H
#define GM_DEVICE_CONFIG_H

#define GM_DEVICE_NAME "ESP32 Example"
#define GM_BOARD_ID 0x3F01
#define GM_PLATFORM "esp32s3"
#define GM_USB_VID 0x1209
#define GM_USB_PID 0x0002
#define GM_N_HUBS 1
#define GM_N_BUTTONS 4
#define GM_N_LEDS 1
#define GM_N_ANALOGS 10
#define GM_N_ENCODERS 1
#define GM_HAS_MATRIX 0
#define GM_MATRIX_KEYS 0
#define GM_MATRIX_VELOCITY 0
#define GM_HAS_I2C 0

#ifndef GM_MIDI_USB
#define GM_MIDI_USB 1
#define GM_MIDI_SERIAL 2
#define GM_MIDI_BLE 3
#endif
#define GM_MIDI_TRANSPORT GM_MIDI_USB

#endif
