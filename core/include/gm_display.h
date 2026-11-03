/*
 * gm_display.h: character displays (displays: in the device YAML).
 *
 * A 14-segment display is a row of characters on output pins (usually a
 * TM1628 hub). Each character is 15 consecutive pins from its first one:
 * segments a b c d e f g1 g2 h i j k l m, then the dot:
 *
 *      ---a---
 *     |\  |  /|
 *     f h i j b
 *     |  \|/  |
 *      -g1 g2-
 *     |  /|\  |
 *     e m l k c
 *     |/  |  \|
 *      ---d---  .dot
 *
 * The core shows the last MIDI message it sent on display 0 (a CC as
 * "<number>.<value>", a note as its name and velocity, a program change as
 * "PG <n>"), and "gemifi" at boot.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GEMIFI_GM_DISPLAY_H
#define GEMIFI_GM_DISPLAY_H

#include <stdint.h>
#include "gm_device.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GM_DISPLAY_SEG14 1

typedef struct {
    uint8_t type;                 /* GM_DISPLAY_SEG14                       */
    uint8_t n_chars;
    const gm_pin_t *chars;        /* left to right: each character's segment a */
} gm_display_t;

/* Segment bits of one character: bit 0 = a ... bit 13 = m, bit 14 = dot.
 * Lower case is shown as upper case; characters with no glyph are blank. */
uint16_t gm_seg14_glyph(char c);

/* Set display d's text. A '.' lights the previous character's dot instead
 * of taking a place. Short text is padded with blanks, long text cut. */
void gm_display_text(uint32_t d, const char *s);

/* Show an outgoing MIDI message on display 0 (status byte, data bytes).
 * Safe from the scan interrupt: it only records the message. */
void gm_display_midi(uint8_t status, uint8_t a, uint8_t b);

/* Main loop: format a recorded message and write changed segments to the
 * pins. The caller flushes the hubs afterwards (vpin_flush_tm1628 ...). */
void gm_display_poll(void);

#ifdef __cplusplus
}
#endif

#endif
