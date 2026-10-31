/*
 * midi-controller: the generic gemifi controller (core/) on ESP32 / ESP8266.
 * The device comes from gm_device_config.h/.c ($GM_DEVICE_DIR, README.md).
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_esp.h"
#include "gm_controller.h"
extern "C" {
#include "gm_device.h"
}

#ifndef GM_FW_VERSION                       /* the device YAML's version: */
#define GM_FW_VERSION 0x00010000u           /* 0.1.0 */
#endif

void setup()
{
    gm_esp_begin();
    uint32_t combo = gm_device.boot_combo;
    int safe = combo && (gm_buttons_now() & combo) == combo;
    gm_init(GM_FW_VERSION, safe);
    gm_esp_scan_start(gm_device.scan_us, gm_scan_tick);
}

void loop()
{
    gm_esp_loop();                          /* MIDI; ESP8266: the scan tick */
    gm_poll();
}
