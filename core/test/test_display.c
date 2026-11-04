/*
 * test_display.c: core/src/display.c on its own: the 14-segment font, text
 * layout (dots, padding, cutting), what an outgoing MIDI message shows,
 * messages (preset names, scrolling) and that only changed segments are
 * written. A fake vpin_write records the pins;
 * the display is 6 characters on hub 1, scattered like the Code 61's.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include "gm_display.h"
#include "gm_vpin.h"

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const gm_pin_t chars[6] = { { 1, 0, 48 }, { 1, 0, 64 }, { 1, 0, 80 }, { 1, 0, 0 }, { 1, 0, 16 }, { 1, 0, 32 } };
const gm_display_t gm_displays[1] = { { GM_DISPLAY_SEG14, 6, chars } };

static uint8_t pin[112];
static int writes;
static uint32_t now;

void vpin_write(gm_pin_t p, int active)
{
    if (p.hub == 1 && p.num < sizeof pin)
        pin[p.num] = (uint8_t)(active != 0);
    writes++;
}

/* Segment bits shown on character i (0 = leftmost), from the fake pins. */
static uint16_t shown(int i)
{
    uint16_t v = 0;
    for (int s = 0; s < 15; s++)
        if (pin[chars[i].num + s])
            v |= (uint16_t)(1u << s);
    return v;
}

#define DOT 0x4000u

/* The display shows text t (with '.' as the previous char's dot). */
static int shows(const char *t)
{
    uint16_t want[6] = { 0 };
    int i = 0;
    for (; *t; t++) {
        if (*t == '.' && i > 0)
            want[i - 1] |= DOT;
        else if (i < 6)
            want[i++] = gm_seg14_glyph(*t);
    }
    for (int k = 0; k < 6; k++)
        if (shown(k) != want[k])
            return 0;
    return 1;
}

int main(void)
{
    /* font */
    CHECK(gm_seg14_glyph('8') == 0x00FF, "8 = a..f + g1 g2 (%04X)", gm_seg14_glyph('8'));
    CHECK(gm_seg14_glyph('1') == (0x0002 | 0x0004 | 0x0400), "1 = b c j (%04X)", gm_seg14_glyph('1'));
    CHECK(gm_seg14_glyph('T') == (0x0001 | 0x0200 | 0x1000), "T = a i l (%04X)", gm_seg14_glyph('T'));
    CHECK(gm_seg14_glyph('#') == (0x0002 | 0x0004 | 0x0040 | 0x0080 | 0x0200 | 0x1000),
          "# = b c g1 g2 i l, no bottom line (%04X)", gm_seg14_glyph('#'));
    CHECK(gm_seg14_glyph('x') == gm_seg14_glyph('X'), "lower case shows as upper case");
    CHECK(gm_seg14_glyph(' ') == 0 && gm_seg14_glyph('~') == 0 && gm_seg14_glyph('\n') == 0,
          "blank, and no glyph = blank");
    int blank = 0;
    for (char c = '0'; c <= 'Z'; c++)
        if ((c <= '9' || c >= 'A') && gm_seg14_glyph(c) == 0)
            blank++;
    CHECK(blank == 0, "every digit and letter has a glyph (%d blank)", blank);

    /* boot text, every segment written once */
    gm_display_poll(now);
    CHECK(shows("GEMIFI"), "boot: GEMIFI");
    CHECK(writes == 6 * 15, "first poll writes every segment (%d)", writes);
    CHECK(pin[48] == 1 && pin[48 + 7] == 1 && pin[48 + 1] == 0, "G on char 1 at bit 48: a, g2 on, b off");

    /* only changes are written; nothing when nothing changes */
    writes = 0;
    gm_display_poll(now);
    CHECK(writes == 0, "no writes while nothing changes (%d)", writes);
    gm_display_text(0, "GEMIFX");
    gm_display_poll(now);
    uint16_t d = (uint16_t)(gm_seg14_glyph('I') ^ gm_seg14_glyph('X'));
    int bits = 0;
    for (int s = 0; s < 15; s++)
        bits += (d >> s) & 1;
    CHECK(writes == bits, "one changed character: %d segment writes (%d)", bits, writes);

    /* layout: dots, padding, cutting */
    gm_display_text(0, "A.B");
    gm_display_poll(now);
    CHECK(shows("A.B") && shown(0) == (gm_seg14_glyph('A') | DOT) && shown(2) == 0, "dot joins A; padded");
    gm_display_text(0, "1234567");
    gm_display_poll(now);
    CHECK(shows("123456"), "long text is cut to 6");
    gm_display_text(0, "123456.");
    gm_display_poll(now);
    CHECK(shown(5) == (gm_seg14_glyph('6') | DOT), "a dot after the last character still shows");
    gm_display_text(0, "..");
    gm_display_poll(now);
    CHECK(shown(0) == DOT && shown(1) == DOT, "a leading dot takes a place (%04X %04X)", shown(0), shown(1));

    /* MIDI */
    gm_display_midi(0xB0, 74, 127);
    gm_display_poll(now);
    CHECK(shows("74 127"), "CC 74 = 127");
    gm_display_midi(0xB3, 102, 5);
    gm_display_poll(now);
    CHECK(shows("102  5"), "CC 102 = 5, any channel");
    gm_display_midi(0xB0, 100, 100);
    gm_display_poll(now);
    CHECK(shows("100.100"), "no gap left: a dot separates them");
    gm_display_midi(0x90, 60, 100);
    gm_display_poll(now);
    CHECK(shows("C4 100"), "note 60 velocity 100 = C4");
    gm_display_midi(0x99, 61, 127);
    gm_display_poll(now);
    CHECK(shows("C#4.127"), "note 61 = C#4");
    gm_display_midi(0x90, 1, 9);
    gm_display_poll(now);
    CHECK(shows("C#-1 9"), "note 1 = C#-1");
    gm_display_midi(0x90, 62, 0);
    gm_display_poll(now);
    CHECK(shows("C#-1 9"), "Note On velocity 0 (a note off) leaves it");
    gm_display_midi(0x80, 62, 0);
    gm_display_midi(0xE0, 0, 64);
    gm_display_poll(now);
    CHECK(shows("C#-1 9"), "Note Off and pitch bend leave it");
    gm_display_midi(0xC0, 12, 0);
    gm_display_poll(now);
    CHECK(shows("PG  12"), "program change 12");
    gm_display_midi(0xB0, 7, 0);
    gm_display_midi(0xB0, 7, 1);
    gm_display_poll(now);
    CHECK(shows("7    1"), "two messages before a poll: the last shows");

    /* messages: shown over MIDI; long ones scroll once and end at the start */
    gm_display_message("CH 5");
    gm_display_poll(now);
    CHECK(shows("CH 5"), "a short message");
    gm_display_midi(0xC0, 3, 0);    /* e.g. the preset's Program Change, same poll */
    gm_display_message("Piano");
    gm_display_poll(now);
    CHECK(shows("PIANO"), "a message wins over MIDI recorded before the same poll");
    gm_display_midi(0x90, 60, 100);
    gm_display_poll(now);
    CHECK(shows("C4 100"), "the next MIDI message replaces it");
    gm_display_message("Preset 2");  /* 8 characters on 6: scrolls 2 places */
    gm_display_poll(now);
    CHECK(shows("PRESET"), "long message: the start first");
    now += 999;
    gm_display_poll(now);
    CHECK(shows("PRESET"), "held 1 s");
    now += 1;
    gm_display_poll(now);
    CHECK(shows("RESET "), "then one character");
    now += 300;
    gm_display_poll(now);
    CHECK(shows("ESET 2"), "and the next, 300 ms later: the end");
    now += 999;
    gm_display_poll(now);
    CHECK(shows("ESET 2"), "the end held 1 s");
    now += 1;
    gm_display_poll(now);
    CHECK(shows("PRESET"), "then back to the start");
    now += 5000;
    gm_display_poll(now);
    CHECK(shows("PRESET"), "and it stays there");
    gm_display_message("A long name here");
    gm_display_poll(now);
    now += 1000;
    gm_display_poll(now);
    CHECK(shows(" LONG "), "scrolling (got %04X)", shown(0));
    gm_display_midi(0xB0, 7, 9);
    gm_display_poll(now);
    now += 300;
    gm_display_poll(now);
    CHECK(shows("7    9"), "MIDI stops a scroll");
    gm_display_message("1.2");
    gm_display_poll(now);
    CHECK(shown(1) == DOT && shown(2) == gm_seg14_glyph('2'), "a message's dot takes a place");

    /* FB_TEXT (gm_host_on_text): covers the device's own text until released */
    gm_display_midi(0xB0, 1, 2);
    gm_display_poll(now);
    CHECK(shows("1    2"), "the device's own text first");
    gm_host_on_text(0, 0, 0, (const uint8_t *)"HOST", 4);
    gm_display_poll(now);
    CHECK(shows("HOST  "), "host text from column 0, the rest blank");
    gm_display_midi(0xB0, 3, 4);
    gm_display_message("NOPE");
    gm_display_poll(now);
    CHECK(shows("HOST  "), "the device's MIDI and messages don't cover it");
    gm_host_on_text(0, 0, 4, (const uint8_t *)"12345", 5);
    gm_display_poll(now);
    CHECK(shows("HOST12"), "from column 4, clipped at the end of the row");
    gm_host_on_text(0, 0, 1, (const uint8_t *)".", 1);
    gm_display_poll(now);
    CHECK(shown(1) == DOT && shown(0) == gm_seg14_glyph('H'), "'.' takes a place of its own");
    gm_host_on_text(0, 0, 0, (const uint8_t *)"\x01", 1);
    gm_display_poll(now);
    CHECK(shown(0) == 0, "a control character shows blank");
    int w = writes;
    gm_host_on_text(0, 1, 0, (const uint8_t *)"XX", 2);     /* no row 1 */
    gm_host_on_text(0, 0, 6, (const uint8_t *)"XX", 2);     /* past the end */
    gm_host_on_text(1, 0, 0, (const uint8_t *)"XX", 2);     /* no display 1 */
    gm_display_poll(now);
    CHECK(writes == w, "a bad row, column or display is dropped");
    gm_display_host_release();
    gm_display_poll(now);
    CHECK(shows("NOPE  "), "released: the device's own text (the last message) again");

    printf("test_display: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
