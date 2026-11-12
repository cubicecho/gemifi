/*
 * gm_esp.h: the gemifi ESP platform (ESP32 classic / S2 / S3, ESP8266),
 * Arduino framework. Implements core/include/gm_hal.h and config_store.h.
 *
 * Build-time settings come from the device's gm_device_config.h when it is
 * on the include path (midi-controller), else the defaults below (hello):
 *   GM_MIDI_TRANSPORT  GM_MIDI_USB (S2/S3 native USB), GM_MIDI_SERIAL
 *                      (raw MIDI bytes on UART0), GM_MIDI_BLE (ESP32, S3)
 *   GM_MIDI_BAUD       serial transport baud rate (115200)
 *   GM_I2C_SDA/SCL     I2C pins for MCP23017 / PCF8574 hubs (GPIO numbers)
 *   GM_BOARD_ID, GM_DEVICE_NAME, GM_USB_VID, GM_USB_PID
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_GM_ESP_H
#define GEMIFI_GM_ESP_H

#include <Arduino.h>
#include <stdint.h>

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
#elif defined(ARDUINO_USB_MODE) && ARDUINO_USB_MODE != 0
#error "USB MIDI needs TinyUSB: build with -DARDUINO_USB_MODE=0"
#endif
#elif GM_MIDI_TRANSPORT == GM_MIDI_BLE
#if !defined(ESP32) || defined(CONFIG_IDF_TARGET_ESP32S2)
#error "midi.transport ble needs an ESP32 or ESP32-S3"
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

/* setup(): ADC, I2C, MIDI transport. */
void gm_esp_begin(void);
/* Call tick() every period_us. ESP32: from a task pinned to the caller's
 * core (above loop() priority). ESP8266: from gm_esp_loop(), paced with
 * micros() (see README). */
void gm_esp_scan_start(uint32_t period_us, void (*tick)(void));
/* loop(): MIDI in/out, and the ESP8266 scan pacing. */
void gm_esp_loop(void);

/* internal */
void gm_esp_scan_service(void);
void gm_esp_midi_begin(void);
uint32_t gm_crc32(uint32_t crc, const uint8_t *p, uint32_t n);   /* zlib */

#ifdef __cplusplus
}
#endif

#endif
