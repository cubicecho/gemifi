/*
 * midi-controller: the generic gemifi controller (core/) on the ESP32 / S2 /
 * S3, ESP-IDF. The device comes from gm_device_config.h/.c, generated from
 * the device YAML ($GM_DEVICE) or given pre-generated ($GM_DEVICE_DIR);
 * README.md.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_esp.h"
#include "gm_controller.h"
#include "gm_device.h"

#ifndef GM_FW_VERSION                       /* the device YAML's version: */
#define GM_FW_VERSION 0x00010000u           /* 0.1.0 */
#endif

/* The main task (sdkconfig: 8 KB of stack, on core 1 where there are two)
 * runs the loop and never returns. */
void app_main(void)
{
    gm_esp_begin();
    uint32_t combo = gm_device.boot_combo;
    int safe = combo && (gm_buttons_now() & combo) == combo;
    gm_init(GM_FW_VERSION, safe);
    gm_esp_scan_start(gm_device.scan_us, gm_scan_tick);
    for (;;) {
        gm_esp_loop();                      /* MIDI in and out */
        gm_poll();
    }
}
