/*
 * display.c: character displays (gm_display.h).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include "gm_display.h"
#include "gm_device_config.h"
#include "gm_vpin.h"

#ifndef GM_N_DISPLAYS
#define GM_N_DISPLAYS 0                 /* a config generated before displays */
#endif

/* Segment bits (gm_display.h): a b c d e f g1 g2 h i j k l m, dot. */
#define SA  0x0001u
#define SB  0x0002u
#define SC  0x0004u
#define SD  0x0008u
#define SE  0x0010u
#define SF  0x0020u
#define SG1 0x0040u
#define SG2 0x0080u
#define SH  0x0100u   /* upper left diagonal  */
#define SI  0x0200u   /* upper centre         */
#define SJ  0x0400u   /* upper right diagonal */
#define SK  0x0800u   /* lower right diagonal */
#define SL  0x1000u   /* lower centre         */
#define SM  0x2000u   /* lower left diagonal  */
#define SDOT 0x4000u
#define SG  (SG1 | SG2)

/* ' ' (0x20) .. '_' (0x5F). */
static const uint16_t font[64] = {
    0,                                  /* ' ' */
    SB | SC | SDOT,                     /* !   */
    SF | SI,                            /* "   */
    SB | SC | SG | SI | SL,             /* #: two bars, middle line */
    SA | SC | SD | SF | SG | SI | SL,   /* $   */
    SC | SF | SG | SJ | SM,             /* %   */
    SA | SD | SE | SG1 | SH | SJ | SK,  /* &   */
    SI,                                 /* '   */
    SJ | SK,                            /* (   */
    SH | SM,                            /* )   */
    SG | SH | SI | SJ | SK | SL | SM,   /* *   */
    SG | SI | SL,                       /* +   */
    SM,                                 /* ,   */
    SG,                                 /* -   */
    SDOT,                               /* .   */
    SJ | SM,                            /* /   */
    SA | SB | SC | SD | SE | SF | SJ | SM,  /* 0 */
    SB | SC | SJ,                       /* 1   */
    SA | SB | SD | SE | SG,             /* 2   */
    SA | SB | SC | SD | SG2,            /* 3   */
    SB | SC | SF | SG,                  /* 4   */
    SA | SC | SD | SF | SG,             /* 5   */
    SA | SC | SD | SE | SF | SG,        /* 6   */
    SA | SB | SC,                       /* 7   */
    SA | SB | SC | SD | SE | SF | SG,   /* 8   */
    SA | SB | SC | SD | SF | SG,        /* 9   */
    SI | SL,                            /* :   */
    SI | SM,                            /* ;   */
    SJ | SK,                            /* <   */
    SD | SG,                            /* =   */
    SH | SM,                            /* >   */
    SA | SB | SG2 | SL,                 /* ?   */
    SA | SB | SD | SE | SF | SG2 | SI,  /* @   */
    SA | SB | SC | SE | SF | SG,        /* A   */
    SA | SB | SC | SD | SG2 | SI | SL,  /* B   */
    SA | SD | SE | SF,                  /* C   */
    SA | SB | SC | SD | SI | SL,        /* D   */
    SA | SD | SE | SF | SG1,            /* E   */
    SA | SE | SF | SG1,                 /* F   */
    SA | SC | SD | SE | SF | SG2,       /* G   */
    SB | SC | SE | SF | SG,             /* H   */
    SA | SD | SI | SL,                  /* I   */
    SB | SC | SD | SE,                  /* J   */
    SE | SF | SG1 | SJ | SK,            /* K   */
    SD | SE | SF,                       /* L   */
    SB | SC | SE | SF | SH | SJ,        /* M   */
    SB | SC | SE | SF | SH | SK,        /* N   */
    SA | SB | SC | SD | SE | SF,        /* O   */
    SA | SB | SE | SF | SG,             /* P   */
    SA | SB | SC | SD | SE | SF | SK,   /* Q   */
    SA | SB | SE | SF | SG | SK,        /* R   */
    SA | SC | SD | SF | SG,             /* S   */
    SA | SI | SL,                       /* T   */
    SB | SC | SD | SE | SF,             /* U   */
    SE | SF | SJ | SM,                  /* V   */
    SB | SC | SE | SF | SK | SM,        /* W   */
    SH | SJ | SK | SM,                  /* X   */
    SH | SJ | SL,                       /* Y   */
    SA | SD | SJ | SM,                  /* Z   */
    SA | SD | SE | SF,                  /* [   */
    SH | SK,                            /* \   */
    SA | SB | SC | SD,                  /* ]   */
    SM | SK,                            /* ^ (as a small caret) */
    SD,                                 /* _   */
};

uint16_t gm_seg14_glyph(char c)
{
    if (c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    if (c < 0x20 || c > 0x5F)
        return 0;
    return font[c - 0x20];
}

#if GM_N_DISPLAYS > 0

#define MAXC 16                         /* the builder allows at most 16 */

extern const gm_display_t gm_displays[GM_N_DISPLAYS];   /* generated config */

static uint16_t want[GM_N_DISPLAYS][MAXC];  /* segments the device shows   */
static uint16_t host[GM_N_DISPLAYS][MAXC];  /* segments a host set (FB_TEXT) */
static uint8_t host_on[GM_N_DISPLAYS];      /* the host's text is showing   */
static uint16_t shown[GM_N_DISPLAYS][MAXC]; /* segments written to the pins */
static uint8_t dirty;
static uint8_t inited;
static volatile uint32_t midi_msg;      /* seq << 24 | status << 16 | a << 8 | b */
static uint32_t midi_seen;

#define MSG_MAX 24u
#define MSG_HOLD_MS 1000u               /* a scrolling message: start and end */
#define MSG_STEP_MS 300u                /* ... and one character */
static char msg[MSG_MAX + 1];
static uint8_t msg_len, msg_pos;
static uint8_t msg_new;                 /* set by gm_display_message */
static uint8_t msg_scroll;              /* 1 scrolling, 2 holding the end */
static uint32_t msg_t;

void gm_display_text(uint32_t d, const char *s)
{
    if (d >= GM_N_DISPLAYS)
        return;
    uint32_t n = gm_displays[d].n_chars, i = 0;
    if (n > MAXC)
        n = MAXC;
    while (*s && i <= n) {
        if (*s == '.' && i > 0 && !(want[d][i - 1] & SDOT)) {
            want[d][i - 1] |= SDOT;
        } else {
            if (i == n)
                break;
            want[d][i++] = gm_seg14_glyph(*s);
        }
        s++;
    }
    while (i < n)
        want[d][i++] = 0;
    dirty = 1;
}

void gm_display_midi(uint8_t status, uint8_t a, uint8_t b)
{
    uint32_t seq = (midi_msg >> 24) + 1u;
    midi_msg = (seq << 24) | ((uint32_t)status << 16) | ((uint32_t)a << 8) | b;
}

void gm_display_message(const char *s)
{
    uint32_t n = 0;
    while (s[n] && n < MSG_MAX) {
        msg[n] = s[n];
        n++;
    }
    msg[n] = 0;
    msg_len = (uint8_t)n;
    msg_new = 1;
}

/* Characters pos.. of the message, every one taking a place. */
static void msg_show(uint32_t pos)
{
    uint32_t n = gm_displays[0].n_chars > MAXC ? MAXC : gm_displays[0].n_chars;
    for (uint32_t i = 0; i < n; i++)
        want[0][i] = pos + i < msg_len ? gm_seg14_glyph(msg[pos + i]) : 0;
    dirty = 1;
}

/* Decimal digits of v (0..999) into p, no padding; returns the count. */
static uint32_t dec(char *p, uint32_t v)
{
    char t[4];
    uint32_t k = 0, n = 0;
    do { t[k++] = (char)('0' + v % 10u); v /= 10u; } while (v && k < 3);
    while (k)
        p[n++] = t[--k];
    return n;
}

/* Left text, then right text right-aligned, on display 0; a dot ends the
 * left part when the two would touch. The right part is left out when it
 * doesn't fit. */
static void show_pair(const char *l, uint32_t nl, const char *r, uint32_t nr)
{
    char s[MAXC * 2 + 2];
    uint32_t n = gm_displays[0].n_chars, j = 0;
    if (n > MAXC)
        n = MAXC;
    for (uint32_t i = 0; i < nl && j < n; i++)
        s[j++] = l[i];
    uint32_t cells = j;
    if (nr && cells + nr <= n) {
        if (cells + nr == n)
            s[j++] = '.';                   /* no gap left: a dot separates them */
        for (uint32_t i = cells; i + nr < n; i++)
            s[j++] = ' ';
        for (uint32_t i = 0; i < nr; i++)
            s[j++] = r[i];
    }
    s[j] = 0;
    gm_display_text(0, s);
}

static void format_midi(uint32_t m)
{
    static const char names[12][2] = { {'C', 0}, {'C', '#'}, {'D', 0}, {'D', '#'}, {'E', 0}, {'F', 0},
                                        {'F', '#'}, {'G', 0}, {'G', '#'}, {'A', 0}, {'A', '#'}, {'B', 0} };
    uint8_t st = (uint8_t)(m >> 16), a = (uint8_t)(m >> 8), b = (uint8_t)m;
    char l[6], r[4];
    uint32_t nl = 0, nr;
    switch (st & 0xF0u) {
    case 0xB0:                              /* CC: number.value */
        nl = dec(l, a);
        nr = dec(r, b);
        show_pair(l, nl, r, nr);
        break;
    case 0x90:                              /* note on: name, velocity */
        if (b == 0)
            return;
        l[nl++] = names[a % 12u][0];
        if (names[a % 12u][1])
            l[nl++] = '#';
        if (a < 12u)
            l[nl++] = '-';
        nl += dec(l + nl, a < 12u ? 1u : a / 12u - 1u);
        nr = dec(r, b);
        show_pair(l, nl, r, nr);
        break;
    case 0xC0:                              /* program change */
        l[0] = 'P';
        l[1] = 'G';
        nr = dec(r, a);
        show_pair(l, 2, r, nr);
        break;
    default:
        break;
    }
}

void gm_display_poll(uint32_t now)
{
    if (!inited) {
        inited = 1;
        for (uint32_t d = 0; d < GM_N_DISPLAYS; d++)
            for (uint32_t i = 0; i < MAXC; i++)
                shown[d][i] = 0x8000u;      /* not written yet: write every segment */
        if (!dirty)
            gm_display_text(0, "gemifi");
    }
    uint32_t m = midi_msg;
    if (m != midi_seen) {
        midi_seen = m;
        msg_scroll = 0;
        format_midi(m);
    }
    if (msg_new) {                      /* a message wins over MIDI in the same poll */
        msg_new = 0;
        msg_pos = 0;
        msg_t = now;
        msg_scroll = msg_len > gm_displays[0].n_chars ? 1u : 0u;
        msg_show(0);
    } else if (msg_scroll && now - msg_t >= (msg_pos == 0 || msg_scroll == 2 ? MSG_HOLD_MS : MSG_STEP_MS)) {
        msg_t = now;
        if (msg_scroll == 2) {
            msg_scroll = 0;             /* done: back to the start */
            msg_pos = 0;
        } else if (++msg_pos + gm_displays[0].n_chars >= msg_len) {
            msg_scroll = 2;             /* the end: hold it */
        }
        msg_show(msg_pos);
    }
    if (!dirty)
        return;
    dirty = 0;
    for (uint32_t d = 0; d < GM_N_DISPLAYS; d++) {
        const gm_display_t *dp = &gm_displays[d];
        uint32_t n = dp->n_chars > MAXC ? MAXC : dp->n_chars;
        for (uint32_t i = 0; i < n; i++) {
            uint16_t w = host_on[d] ? host[d][i] : want[d][i];
            uint16_t diff = (shown[d][i] & 0x8000u) ? 0x7FFFu : (uint16_t)(w ^ shown[d][i]);
            if (!diff)
                continue;
            shown[d][i] = w;
            for (uint32_t s = 0; s < 15u; s++) {
                if (!(diff & (1u << s)))
                    continue;
                gm_pin_t p = dp->chars[i];
                p.num = (uint16_t)(p.num + s);
                vpin_write(p, (w >> s) & 1u);
            }
        }
    }
}

/* FB_TEXT (sysex.md 4.6.2): from (row, col), every character taking a
 * place, clipped at the end of the row; nothing else changes. The host's
 * text covers the device's own (MIDI values, messages), which carries on
 * underneath and shows again after gm_display_host_release. These displays
 * have one row. */
void gm_host_on_text(uint32_t d, uint32_t row, uint32_t col, const uint8_t *text, uint32_t n)
{
    if (d >= GM_N_DISPLAYS || row != 0)
        return;
    uint32_t w = gm_displays[d].n_chars > MAXC ? MAXC : gm_displays[d].n_chars;
    if (col >= w)
        return;
    if (!host_on[d]) {
        host_on[d] = 1;
        for (uint32_t i = 0; i < MAXC; i++)
            host[d][i] = 0;
    }
    for (uint32_t i = 0; i < n && col + i < w; i++) {
        uint8_t c = text[i];
        host[d][col + i] = c >= 0x20 && c <= 0x7E ? gm_seg14_glyph((char)c) : 0;
    }
    dirty = 1;
}

void gm_display_host_release(void)
{
    for (uint32_t d = 0; d < GM_N_DISPLAYS; d++)
        host_on[d] = 0;
    dirty = 1;
}

#else

void gm_host_on_text(uint32_t d, uint32_t row, uint32_t col, const uint8_t *text, uint32_t n)
{
    (void)d;
    (void)row;
    (void)col;
    (void)text;
    (void)n;
}
void gm_display_host_release(void) {}
void gm_display_text(uint32_t d, const char *s) { (void)d; (void)s; }
void gm_display_midi(uint8_t status, uint8_t a, uint8_t b) { (void)status; (void)a; (void)b; }
void gm_display_message(const char *s) { (void)s; }
void gm_display_poll(uint32_t now) { (void)now; }

#endif
