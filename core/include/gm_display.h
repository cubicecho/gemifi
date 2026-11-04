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
 * "PG <n>"), "gemifi" at boot, and messages (gm_display_message): the
 * preset's name when it is selected, "CH <n>" when the channel changes on
 * the device.
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

/* Show s on display 0 until the next MIDI message (or message). Text
 * longer than the display scrolls once: the start for 1 s, one character
 * every 300 ms, the end for 1 s, then the start again. Every character
 * takes a place ('.' too). Main loop only; s is copied (at most 24). */
void gm_display_message(const char *s);

/* FB_TEXT (sysex.md 4.6.2): text from a host at (row, col) on display d.
 * It covers what the device shows until gm_display_host_release (FB_RELEASE
 * 0 or 3). Also declared in gm_feedback.h, which app_proto.c calls. */
void gm_host_on_text(uint32_t d, uint32_t row, uint32_t col, const uint8_t *text, uint32_t n);
void gm_display_host_release(void);

/* Main loop: format a recorded message, step a scrolling one, and write
 * changed segments to the pins. The caller flushes the hubs afterwards
 * (vpin_flush_tm1628 ...). now = hal_millis(). */
void gm_display_poll(uint32_t now);

#ifdef __cplusplus
}
#endif

#endif
