/*
 * maudio_pads.h: the maudio_pads LED strip platform's config (gm_hub_t
 * .mpads) and state (gm_hub_state_t .u.mpads);
 * components/maudio_pads/README.md. gm_device.h includes it through the
 * generated gm_components.h.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_MAUDIO_PADS_H
#define GEMIFI_MAUDIO_PADS_H

#include <stdint.h>
#include "gm_pin.h"

/* .mpads: the M-Audio Code pad LED board (YAML led_strip: platform:
 * maudio_pads): RGB LEDs behind a serial link with the board's own protocol
 * (maudio_pads.c), 115200 baud on tx. Outputs only, pin n = LED n / 3,
 * colour n % 3 (0 red, 1 green, 2 blue); sent from the main loop, a byte
 * per colour in the hub RAM. */
typedef struct {
    gm_pin_t tx;
    gm_pin_t rx;          /* the board's answers (acks), optional          */
    /* the brightness curve applied first (gamma:; 256 entries,
     * 0 = straight) */
    const uint8_t *curve;
    uint8_t count;        /* LEDs                                          */
    uint8_t dma;          /* send by DMA (dma:), else interrupt            */
    uint8_t white[3];     /* what full red, green and blue send (white:,
                             the white balance; 0 = 255)                   */
} gm_hub_maudio_pads_t;

typedef struct {
    uint32_t t_ka, t_any; /* last A5 keepalive, last write   */
    uint32_t t_led;       /* when the awaited LED went out   */
    uint8_t repainted;    /* every LED sent again once        */
    uint8_t wait_led;     /* rx_pin: the answer awaited, LED + 1, 0 = none */
    uint8_t misses;       /* ... unanswered in a row          */
    uint8_t acks;         /* 0 none yet, 1 answering, 2 gave up */
} gm_maudio_pads_state_t;

#endif
