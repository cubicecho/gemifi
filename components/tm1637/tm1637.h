/*
 * tm1637.h: the tm1637 LED driver platform's config (gm_hub_t .tm37);
 * components/tm1637/README.md. gm_device.h includes it through the
 * generated gm_components.h. Its state is the led_driver base's
 * gm_led_driver_state_t (.u.led_driver, led_driver.h).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_TM1637_H
#define GEMIFI_TM1637_H

#include <stdint.h>
#define GM_LED_DRIVER_BITS 1     /* its pins are the led_driver base's bit pins */
#include "led_driver.h"
#include "gm_pin.h"

/* .tm37: a TM1637 (2-wire: CLK, DIO, no chip select; one chip per pair),
 * bit-banged from the main loop: outputs, pin = grid * 8 + segment (bit 0
 * = SEG1 = a ... bit 7 = SEG8, the point or colon). */
typedef struct {
    gm_pin_t clk;
    gm_pin_t dio;
    uint8_t grids;        /* digits written, 1..6                          */
    uint8_t brightness;   /* 0..7 (display control 0x88 | it)              */
} gm_hub_tm1637_t;

#endif
