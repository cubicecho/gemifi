/*
 * led_driver.h: what the led_driver base gives the platforms with a bit
 * per pin in the hub RAM (max7219, tm1637, ht16k33): their state
 * (gm_hub_state_t .u.led_driver) and pins. components/led_driver/README.md.
 * Their headers define GM_LED_DRIVER_BITS and include it, so the generated
 * gm_components.h has it, and led_driver.c builds the bit pins only then
 * (a device whose only LED driver is a TM1628 has no .u.led_driver).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_LED_DRIVER_H
#define GEMIFI_LED_DRIVER_H

#include <stdint.h>

typedef struct gm_hub_state gm_hub_state_t;     /* gm_hub.h */

typedef struct {
    uint8_t dirty;        /* RAM changed since the last write */
} gm_led_driver_state_t;

/* n bytes of the hub RAM, all off and written whole on the first send
 * (init); pin num = RAM bit, byte * 8 + bit, of `bytes` bytes, marking the
 * chip dirty if it changed (write). */
void gm_led_driver_init(gm_hub_state_t *s, uint32_t n);
void gm_led_driver_write(gm_hub_state_t *s, uint32_t bytes, uint32_t num, int level);

#endif
