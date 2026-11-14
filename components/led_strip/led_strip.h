/*
 * led_strip.h: what the led_strip base gives its platforms (ws2812,
 * maudio_pads): the full-colour output. components/led_strip/README.md.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_LED_STRIP_H
#define GEMIFI_LED_STRIP_H

#include <stdint.h>

/* What goes on the wire for colour c (0 red, 1 green, 2 blue, 3 white) at
 * value v of a strip: through its curve (gamma:), then scaled to its white
 * (white:, 0 = 255). A value above 0 stays above 0. */
uint8_t gm_led_strip_colour_out(const uint8_t *curve, const uint8_t *white, uint32_t c, uint32_t v);

#endif
