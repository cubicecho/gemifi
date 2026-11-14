/*
 * ws2812.h: the ws2812 hub type's config (gm_hub_t .ws) and state
 * (gm_hub_state_t .u.ws); components/ws2812/README.md. gm_device.h
 * includes it through the generated gm_components.h.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_WS2812_H
#define GEMIFI_WS2812_H

#include <stdint.h>
#include "gm_pin.h"

/* .ws: an addressable LED chain (WS2812B, SK6812 RGB or RGBW) on one MCU
 * pin, sent by the platform (gm_hal.h hal_ledstrip_*) from the main loop.
 * Outputs only: pin n = LED n / channels, colour n % channels (0 red,
 * 1 green, 2 blue, 3 white), a byte each in the hub RAM. */
typedef struct {
    gm_pin_t pin;         /* the first LED's data in                       */
    const uint8_t *curve; /* the brightness curve applied first (gamma:;
                             256 entries, 0 = straight)                    */
    uint16_t count;       /* LEDs                                          */
    uint8_t channels;     /* 3 (RGB) or 4 (RGBW)                            */
    uint8_t order;        /* wire order (order:): byte k of an LED is colour
                             (order >> 2k) & 3, GRB = 0x21                 */
    uint8_t white[4];     /* what full red, green, blue, white send
                             (white:, the white balance; 0 = 255)          */
} gm_hub_ws2812_t;

typedef struct {
    uint32_t t_frame;     /* ms the last frame went out       */
    uint8_t dirty;        /* a colour changed since           */
    uint8_t open;         /* hal_ledstrip_open took the pin   */
    uint8_t tried;        /* a frame went out (t_frame is set) */
} gm_ws2812_state_t;

#endif
