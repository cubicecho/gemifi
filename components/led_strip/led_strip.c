/*
 * led_strip.c: the led_strip base, what its platforms (ws2812,
 * maudio_pads) share (led_strip.h; components/led_strip/README.md).
 *
 * SPDX-License-Identifier: MIT
 */
#include "led_strip.h"

uint8_t gm_led_strip_colour_out(const uint8_t *curve, const uint8_t *white, uint32_t c, uint32_t v)
{
    if (curve)
        v = curve[v];
    uint32_t w = white[c] ? white[c] : 255u;
    if (w == 255u || !v)
        return (uint8_t)v;
    v = (v * w + 127u) / 255u;
    return (uint8_t)(v ? v : 1u);
}
