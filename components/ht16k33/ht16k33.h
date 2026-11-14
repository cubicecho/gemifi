/*
 * ht16k33.h: the ht16k33 LED driver platform's config (gm_hub_t .ht);
 * components/ht16k33/README.md. gm_device.h includes it through the
 * generated gm_components.h. Its state is the led_driver base's
 * gm_led_driver_state_t (.u.led_driver, led_driver.h).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_HT16K33_H
#define GEMIFI_HT16K33_H

#include <stdint.h>
#define GM_LED_DRIVER_BITS 1     /* its pins are the led_driver base's bit pins */
#include "led_driver.h"

/* .ht: an HT16K33 LED driver on the I2C bus (the 14-segment and 7-segment
 * backpacks), written from the main loop: outputs, pin = COM * 16 + ROW
 * (display RAM bit: byte * 8 + bit). The key scan isn't read. */
typedef struct {
    uint8_t addr;         /* 7-bit I2C address, 0x70..0x77                 */
    uint8_t bus;          /* the i2c: bus, hal_i2c_*'s bus                 */
    uint8_t brightness;   /* 0..15 (dimming 0xE0 | it)                     */
} gm_hub_ht16k33_t;

#endif
