/*
 * gm_esp.h: the gemifi ESP platform (ESP32 classic / S2 / S3) on ESP-IDF.
 * Implements core/include/gm_hal.h and config_store.h.
 *
 * Build-time settings come from the device's gm_device_config.h when it is
 * on the include path (midi-controller), else the defaults below (hello):
 *   GM_MIDI_TRANSPORT  GM_MIDI_USB (S2/S3 native USB), GM_MIDI_SERIAL
 *                      (raw MIDI bytes on UART0), GM_MIDI_BLE (ESP32, S3)
 *   GM_MIDI_BAUD       serial transport baud rate (115200)
 *   GM_I2C_SDA/SCL     I2C pins for the I2C hubs (GPIO numbers)
 *   GM_BOARD_ID, GM_DEVICE_NAME, GM_USB_VID, GM_USB_PID
 * The transport also picks the sdkconfig fragment (firmware/esp/
 * sdkconfig.<transport>: TinyUSB, NimBLE, the console), in gemifi_esp.py.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_GM_ESP_H
#define GEMIFI_GM_ESP_H

#include <stdint.h>
#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if __has_include("gm_device_config.h")
#include "gm_device_config.h"
#endif

#include "gm_hal.h"                         /* GM_MIDI_USB / _SERIAL / _BLE */

#if defined(CONFIG_IDF_TARGET_ESP32S2) || defined(CONFIG_IDF_TARGET_ESP32S3)
#define GM_ESP_NATIVE_USB 1
#else
#define GM_ESP_NATIVE_USB 0
#endif

#ifndef GM_MIDI_TRANSPORT
#if GM_ESP_NATIVE_USB
#define GM_MIDI_TRANSPORT GM_MIDI_USB
#else
#define GM_MIDI_TRANSPORT GM_MIDI_SERIAL
#endif
#endif

#if GM_MIDI_TRANSPORT == GM_MIDI_USB
#if !GM_ESP_NATIVE_USB
#error "midi.transport usb needs an ESP32-S2 or -S3 (use serial or ble)"
#elif !CONFIG_TINYUSB_MIDI_COUNT
#error "USB MIDI needs TinyUSB's MIDI class: build with firmware/esp/sdkconfig.usb (gemifi_esp.py)"
#endif
#elif GM_MIDI_TRANSPORT == GM_MIDI_BLE
#if !CONFIG_BT_NIMBLE_ENABLED
#error "midi.transport ble needs NimBLE: build with firmware/esp/sdkconfig.ble (gemifi_esp.py), on an ESP32 or ESP32-S3"
#endif
#elif GM_MIDI_TRANSPORT != GM_MIDI_SERIAL
#error "unknown GM_MIDI_TRANSPORT"
#endif

#ifndef GM_MIDI_BAUD
#define GM_MIDI_BAUD 115200
#endif
#ifndef GM_BOARD_ID
#define GM_BOARD_ID 0x3F00          /* DIY range: the hello firmware */
#endif
#ifndef GM_DEVICE_NAME
#define GM_DEVICE_NAME "gemifi hello"
#endif
#ifndef GM_USB_VID
#define GM_USB_VID 0x1209
#endif
#ifndef GM_USB_PID
#define GM_USB_PID 0x0002
#endif

#define GM_ESP_MAX_CHUNK 256

#ifdef __cplusplus
extern "C" {
#endif

/* Start of app_main(): NVS, I2C, the MIDI transport. */
void gm_esp_begin(void);
/* Call tick() every period_us (at least 100), from a task pinned to the
 * caller's core above its priority, so it preempts the main loop as an ISR
 * would (README "Scan timer"). */
void gm_esp_scan_start(uint32_t period_us, void (*tick)(void));
/* Each main-loop pass, before gm_poll(): MIDI in and out. */
void gm_esp_loop(void);

/* microseconds since boot (wraps) */
uint32_t gm_esp_micros(void);

/* internal */
void gm_esp_midi_begin(void);
uint32_t gm_crc32(uint32_t crc, const uint8_t *p, uint32_t n);   /* zlib */

#ifdef __cplusplus
}
#endif

#endif
