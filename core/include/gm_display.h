/*
 * gm_display.h: character displays (displays: in the device YAML).
 *
 * A segment display is a row of characters on output pins of hubs (a text
 * display, GM_DISPLAY_TEXT, a row of characters on an OLED). Each
 * character is its segments' pins, in order: either a run of pins from its
 * first one (chars, the usual wiring: a character's segments on one digit's
 * lines) or a list per character (map: characters on the SEG lines, or any
 * other wiring).
 *
 * A 14-segment character (GM_DISPLAY_SEG14, 15 pins): segments a b c d e f
 * g1 g2 h i j k l m, then the dot:
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
 * A 7-segment character (GM_DISPLAY_SEG7, 8 pins): segments a b c d e f g,
 * then the decimal point:
 *
 *      --a--
 *     f     b
 *      --g--
 *     e     c
 *      --d--  .dp
 *
 * What a display shows while no host is writing to it is its `shows`
 * (GM_SHOWS_*). GM_SHOWS_LAST_MESSAGE (display 0's default): "gemifi" at
 * boot, then the last MIDI message the device sent (a CC as
 * "<number>.<value>", a note as its name and velocity, a program change as
 * "PC <n>") and the device's messages (gm_display_message: the preset's
 * name when it is selected, "CH <n>" when the channel changes on the
 * device). A message or a program change is kept from plain MIDI for 1.5 s
 * and shows over a host's FB_TEXT meanwhile (a long message until it has
 * scrolled once). The others show one value: the active preset's number or name,
 * or the MIDI channel; GM_SHOWS_NONE shows nothing of its own.
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
#define GM_DISPLAY_SEG7  2
/* A text display: one row of 5x7 characters (6 x 8 pixels each, x scale)
 * on an OLED hub (GM_HUB_SSD1306), drawn with vpin_oled_draw. Every
 * printable ASCII character has a glyph, lower case too, and a '.' takes a
 * place. */
#define GM_DISPLAY_TEXT  3

/* gm_display_t.shows: what the display shows of its own (YAML shows:). */
#define GM_SHOWS_LAST_MESSAGE  0      /* MIDI messages and the device's messages */
#define GM_SHOWS_PRESET_NUMBER 1      /* the active preset, 1.., right-aligned  */
#define GM_SHOWS_PRESET_NAME   2      /* its name, scrolled once if long        */
#define GM_SHOWS_CHANNEL       3      /* "CH <n>", or <n> where that won't fit  */
#define GM_SHOWS_NONE          4      /* blank: only a host's text              */

typedef struct {
    uint8_t type;                 /* GM_DISPLAY_SEG14 / GM_DISPLAY_SEG7     */
    uint8_t n_chars;
    /* Left to right: each character's segment a; its other segments are the
     * pins after it (15 for SEG14, 8 for SEG7). Not used with map. */
    const gm_pin_t *chars;
    /* Or every segment's pin: n_chars runs of 15 / 8 pins, left to right, in
     * segment order; GM_NO_PIN = not wired. 0 = chars. */
    const gm_pin_t *map;
    uint8_t shows;                /* GM_SHOWS_*                             */
    /* GM_DISPLAY_TEXT: chars[0] is where the row starts (hub = the OLED,
     * num = its framebuffer byte, page * width + x); each character is
     * 6 x scale pixels wide and 8 x scale tall. */
    uint8_t scale;
} gm_display_t;

/* Segment bits of one character: bit 0 = a ... bit 13 = m, bit 14 = dot.
 * Lower case is shown as upper case; characters with no glyph are blank. */
uint16_t gm_seg14_glyph(char c);

/* A 7-segment character: bit 0 = a ... bit 6 = g, bit 7 = the point.
 * Letters are shown as best 7 segments can (b, d, n, r, t as lower case);
 * characters with no glyph (K, M, W, X ... are approximated, # % & * are
 * not) are blank. */
uint8_t gm_seg7_glyph(char c);

/* A text display's character (GM_DISPLAY_TEXT): its 5 columns, bit 0 at the
 * top; 0x20..0x7E, others blank. */
const uint8_t *gm_text_glyph(char c);

/* Set display d's text. A '.' lights the previous character's dot instead
 * of taking a place (on a text display it takes one). Short text is padded with blanks, long text cut. */
void gm_display_text(uint32_t d, const char *s);

/* Show an outgoing MIDI message on the last_message displays (status
 * byte, data bytes). Safe from the scan interrupt: it only records it. */
void gm_display_midi(uint8_t status, uint8_t a, uint8_t b);

/* Show s on the last_message displays until the next MIDI message (or
 * message). Text longer than a display scrolls once: the start for 1 s, one
 * character every 300 ms, the end for 1 s, then the start again. Every
 * character takes a place ('.' too). Main loop only; s is copied (at most
 * 24). */
void gm_display_message(const char *s);

/* The active preset (1..) and its name (copied, at most 24; 0 = keep the
 * last one), for the preset_number and preset_name displays. Main loop. */
void gm_display_preset(uint32_t number, const char *name);

/* The MIDI channel (1..16), for the channel displays: called before each
 * gm_display_poll; only a change redraws. */
void gm_display_channel(uint32_t channel);

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
