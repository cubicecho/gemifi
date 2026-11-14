/*
 * max7219.h: the max7219 hub type's config (gm_hub_t .mx);
 * components/max7219/README.md. gm_device.h includes it through the
 * generated gm_components.h. Its state is the hub base's u.seg (gm_hub.h).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_MAX7219_H
#define GEMIFI_MAX7219_H

#include <stdint.h>
#include "gm_pin.h"

/* .mx: a chain of MAX7219 LED drivers (MAX7221 too), bit-banged from the
 * main loop: outputs, pin = chip * 64 + digit * 8 + segment (0..6 = A..G,
 * 7 = DP; digit 0 = DIG0). Chip 0 is the one whose DIN is the MCU's. The
 * chips' own decode is off: the core sends segments. */
typedef struct {
    gm_pin_t clk;
    gm_pin_t din;
    gm_pin_t cs;          /* LOAD / CS: the words are taken on its rising edge */
    uint8_t chips;        /* in the chain, 1..8                            */
    uint8_t intensity;    /* 0..15 (register 0x0A)                         */
    uint8_t scan_limit;   /* digits scanned, 1..8 (register 0x0B: it - 1)  */
} gm_hub_max7219_t;

#endif
