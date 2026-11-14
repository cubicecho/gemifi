/*
 * gm_pin.h: a pin of a gemifi device (gm_pin_t), on the MCU or on a hub.
 * Its own header so components can name pins without gm_device.h.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_GM_PIN_H
#define GEMIFI_GM_PIN_H

#include <stdint.h>

/* ---- virtual pins ----------------------------------------------------- */
/* hub 0 = MCU pin (num = platform pin number: NUC1xx port * 16 + bit, e.g.
 * PB12 = 0x1C; ESP32 GPIO number). hub n >= 1 = gm_device.hubs[n-1],
 * num = that hub's pin / channel (chained chips continue: 8..15 = 2nd chip).
 * TM1628 hubs: num = display RAM bit, byte * 8 + bit (37 = byte 4, bit 5).
 * GM_PIN_INVERT: the signal is active-low (pressed / lit = electrical 0). */
typedef struct {
    uint8_t hub;
    uint8_t flags;
    uint16_t num;
} gm_pin_t;

#define GM_HUB_NONE     0xFFu        /* "no pin" */
#define GM_PIN_INVERT   (1u << 0)
#define GM_PIN_PULLUP   (1u << 1)    /* MCU pins: enable the internal pull-up   */
#define GM_PIN_PULLDOWN (1u << 2)    /* MCU pins: enable the internal pull-down */
#define GM_PIN_OPEN_DRAIN (1u << 3)  /* MCU outputs: open-drain (TM1628 DIO)     */
#define GM_NO_PIN       { GM_HUB_NONE, 0, 0 }

#endif
