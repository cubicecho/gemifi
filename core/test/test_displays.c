/*
 * test_displays.c: core/src/display.c with several displays (displays-cfg/):
 * the 7-segment font and point, a per-character segment map, what each
 * display shows of its own (shows:), and which display gets the device's
 * messages and a host's text. A fake vpin_write records the pins per hub:
 *   0: 3 x segment7 on hub 1, first pins 0, 8, 16: the preset number;
 *   1: 2 x segment7 on hub 2, characters on the SEG lines (segment k of
 *      character i at k * 16 + i), no point wired: the channel;
 *   2: 6 x segment14 on hub 3, from 0, 15, ...: the last message;
 *   3: 4 x segment7 on hub 4: the preset's name;
 *   4: 5 x segment7 on hub 5: the channel again (room for "CH");
 *   5: 2 x segment7 on hub 6: none.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include "gm_display.h"
#include "gm_vpin.h"

static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define NO { GM_HUB_NONE, 0, 0 }
static const gm_pin_t c0[3] = { { 1, 0, 0 }, { 1, 0, 8 }, { 1, 0, 16 } };
static const gm_pin_t m1[16] = {
    { 2, 0, 0 }, { 2, 0, 16 }, { 2, 0, 32 }, { 2, 0, 48 }, { 2, 0, 64 }, { 2, 0, 80 }, { 2, 0, 96 }, NO,
    { 2, 0, 1 }, { 2, 0, 17 }, { 2, 0, 33 }, { 2, 0, 49 }, { 2, 0, 65 }, { 2, 0, 81 }, { 2, 0, 97 }, NO,
};
static const gm_pin_t c2[6] = { { 3, 0, 0 }, { 3, 0, 15 }, { 3, 0, 30 }, { 3, 0, 45 }, { 3, 0, 60 }, { 3, 0, 75 } };
static const gm_pin_t c3[4] = { { 4, 0, 0 }, { 4, 0, 8 }, { 4, 0, 16 }, { 4, 0, 24 } };
static const gm_pin_t c4[5] = { { 5, 0, 0 }, { 5, 0, 8 }, { 5, 0, 16 }, { 5, 0, 24 }, { 5, 0, 32 } };
static const gm_pin_t c5[2] = { { 6, 0, 0 }, { 6, 0, 8 } };
const gm_display_t gm_displays[6] = {
    { GM_DISPLAY_SEG7, 3, c0, 0, GM_SHOWS_PRESET_NUMBER, 0 },
    { GM_DISPLAY_SEG7, 2, 0, m1, GM_SHOWS_CHANNEL, 0 },
    { GM_DISPLAY_SEG14, 6, c2, 0, GM_SHOWS_LAST_MESSAGE, 0 },
    { GM_DISPLAY_SEG7, 4, c3, 0, GM_SHOWS_PRESET_NAME, 0 },
    { GM_DISPLAY_SEG7, 5, c4, 0, GM_SHOWS_CHANNEL, 0 },
    { GM_DISPLAY_SEG7, 2, c5, 0, GM_SHOWS_NONE, 0 },
};

static uint8_t pin[7][128];
static int writes[7], bad_writes;
static uint32_t now;

void vpin_write(gm_pin_t p, int active)
{
    if (p.hub >= 1 && p.hub <= 6 && p.num < 128) {
        pin[p.hub][p.num] = (uint8_t)(active != 0);
        writes[p.hub]++;
    } else {
        bad_writes++;
    }
}

/* Segment bits shown on character i of display d, from the fake pins. */
static uint16_t shown(int d, int i)
{
    const gm_display_t *dp = &gm_displays[d];
    int ns = dp->type == GM_DISPLAY_SEG7 ? 8 : 15;
    uint16_t v = 0;
    for (int s = 0; s < ns; s++) {
        gm_pin_t p = dp->map ? dp->map[i * ns + s] : dp->chars[i];
        if (p.hub == GM_HUB_NONE)
            continue;
        if (!dp->map)
            p.num = (uint16_t)(p.num + s);
        if (pin[p.hub][p.num])
            v |= (uint16_t)(1u << s);
    }
    return v;
}

/* Display d shows text t, every character taking a place except a '.'
 * after one (its dot). */
static int shows(int d, const char *t)
{
    const gm_display_t *dp = &gm_displays[d];
    uint16_t want[16] = { 0 };
    int i = 0, n = dp->n_chars;
    uint16_t dot = dp->type == GM_DISPLAY_SEG7 ? 0x80 : 0x4000;
    for (; *t; t++) {
        if (*t == '.' && i > 0)
            want[i - 1] |= dot;
        else if (i < n)
            want[i++] = dp->type == GM_DISPLAY_SEG7 ? gm_seg7_glyph(*t) : gm_seg14_glyph(*t);
    }
    for (int k = 0; k < n; k++)
        if (shown(d, k) != want[k])
            return 0;
    return 1;
}

static void poll(void)
{
    gm_display_poll(now);
}

int main(void)
{
    /* the 7-segment font: a = bit 0 .. g = bit 6, the point bit 7 */
    CHECK(gm_seg7_glyph('8') == 0x7F, "8 = a..g (%02X)", gm_seg7_glyph('8'));
    CHECK(gm_seg7_glyph('0') == 0x3F && gm_seg7_glyph('1') == 0x06 && gm_seg7_glyph('7') == 0x07,
          "0 1 7 (%02X %02X %02X)", gm_seg7_glyph('0'), gm_seg7_glyph('1'), gm_seg7_glyph('7'));
    CHECK(gm_seg7_glyph('-') == 0x40 && gm_seg7_glyph('_') == 0x08 && gm_seg7_glyph('.') == 0x80, "- _ .");
    CHECK(gm_seg7_glyph('b') == gm_seg7_glyph('B') && gm_seg7_glyph('B') == 0x7C, "b in lower case");
    CHECK(gm_seg7_glyph('#') == 0 && gm_seg7_glyph('~') == 0 && gm_seg7_glyph('\n') == 0, "no glyph: blank");
    int blank = 0;
    for (char c = '0'; c <= 'Z'; c++)
        if ((c <= '9' || c >= 'A') && gm_seg7_glyph(c) == 0)
            blank++;
    CHECK(blank == 0, "every digit and letter has a 7-segment glyph (%d blank)", blank);

    /* boot: each display its own value; display 2 (last_message) "gemifi" */
    gm_display_preset(1, "Clean tone");
    gm_display_channel(1);
    poll();
    CHECK(shows(0, "  1"), "preset number, right-aligned (%02X %02X %02X)", shown(0, 0), shown(0, 1), shown(0, 2));
    CHECK(shows(1, " 1"), "channel on 2 characters: the number");
    CHECK(shows(2, "GEMIFI"), "last_message: the boot text");
    CHECK(shows(3, "CLEA"), "preset name: its start");
    CHECK(shows(4, "CH  1"), "channel on 5 characters: CH and the number");
    CHECK(shows(5, "  "), "none: blank");
    CHECK(bad_writes == 0, "no write to the map's unwired point (%d)", bad_writes);
    /* the map: segment k of character i at k * 16 + i on hub 2 */
    CHECK(pin[2][16 + 1] && pin[2][32 + 1] && !pin[2][0 + 1] && !pin[2][0] && !pin[2][16],
          "'1' in character 1: b (17) and c (33); character 0 blank");

    /* only changed segments are written */
    int w0 = writes[1], w2 = writes[3];
    poll();
    CHECK(writes[1] == w0 && writes[3] == w2, "nothing changed: nothing written");

    /* MIDI and messages go to the last_message display only */
    gm_display_midi(0xB0, 7, 9);
    poll();
    CHECK(shows(2, "7    9"), "a CC on display 2");
    CHECK(shows(0, "  1") && shows(3, "CLEA") && shows(5, "  "), "the others keep theirs");
    gm_display_message("CH 5");
    poll();
    CHECK(shows(2, "CH 5  ") && shows(0, "  1"), "a message on display 2 only");

    /* the name scrolls once: start 1 s, a character every 300 ms, end 1 s */
    now += 1000;
    poll();
    CHECK(shows(3, "LEAN"), "name scrolls (%02X)", shown(3, 0));
    now += 300;
    poll();
    CHECK(shows(3, "EAN "), "one more");
    for (int k = 0; k < 20; k++) {
        now += 300;
        poll();
    }
    CHECK(shows(3, "CLEA"), "then back to its start");

    /* a preset switch: number and name */
    w2 = writes[3];
    gm_display_preset(12, "Lead");
    poll();
    CHECK(shows(0, " 12") && shows(3, "LEAD"), "preset 12, Lead");
    gm_display_preset(123, 0);
    poll();
    CHECK(shows(0, "123") && shows(3, "LEAD"), "3 digits; name kept (0)");
    gm_display_preset(1234, "");
    poll();
    CHECK(shows(0, "234") && shows(3, "    "), "too wide: the last digits; empty name blank");

    /* the channel: only a change redraws */
    w0 = writes[2];
    gm_display_channel(1);
    poll();
    CHECK(writes[2] == w0, "same channel: nothing written");
    gm_display_channel(12);
    poll();
    CHECK(shows(1, "12") && shows(4, "CH 12"), "channel 12");
    gm_display_channel(16);
    poll();
    CHECK(shows(4, "CH 16"), "channel 16");

    /* text and the point on 7 segments */
    gm_display_text(0, "1.2");
    poll();
    CHECK(shown(0, 0) == (0x06 | 0x80) && shown(0, 1) == gm_seg7_glyph('2'), "'.' lights the point");
    CHECK(pin[1][7], "the point is pin 7 of a character");
    gm_display_text(0, "..");
    poll();
    CHECK(shown(0, 0) == 0x80 && shown(0, 1) == 0x80, "a '.' after a point takes a place");
    gm_display_text(1, "8.8.");
    poll();
    CHECK(shown(1, 0) == 0x7F && shown(1, 1) == 0x7F && bad_writes == 0,
          "an unwired point: not written, the rest shows");

    /* FB_TEXT by display index; FB_RELEASE hands them all back */
    gm_host_on_text(0, 0, 0, (const uint8_t *)"Hi", 2);
    gm_host_on_text(5, 0, 1, (const uint8_t *)"7", 1);
    poll();
    CHECK(shows(0, "HI ") && shows(5, " 7"), "host text on displays 0 and 5");
    gm_display_preset(5, "Solo");
    gm_display_midi(0xC0, 4, 0);
    poll();
    CHECK(shows(0, "HI ") && shows(3, "SOLO") && shows(2, "PC   4"), "the host's text covers the device's");
    gm_host_on_text(6, 0, 0, (const uint8_t *)"X", 1);   /* no display 6 */
    gm_host_on_text(1, 1, 0, (const uint8_t *)"X", 1);   /* one row */
    gm_display_host_release();
    poll();
    CHECK(shows(0, "  5") && shows(5, "  "), "released: preset 5, blank again");
    CHECK(bad_writes == 0, "no stray writes (%d)", bad_writes);

    printf("test_displays: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
