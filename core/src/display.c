/*
 * display.c: character displays (gm_display.h): the 14- and 7-segment fonts
 * and a 5x7 one for text displays (OLEDs), what each display shows of its
 * own (shows:), and the changed segments to their pins (characters to the
 * OLED's framebuffer).
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


/* 7 segments: a b c d e f g, then the point. */
#define S7A  0x01u
#define S7B  0x02u
#define S7C  0x04u
#define S7D  0x08u
#define S7E  0x10u
#define S7F  0x20u
#define S7G  0x40u
#define S7DP 0x80u

/* ' ' (0x20) .. '_' (0x5F); what 7 segments can't show is blank. */
static const uint8_t font7[64] = {
    0, S7B | S7C | S7DP, S7B | S7F, 0, 0x6D, 0, 0, S7B,        /*  !"#$%&' */
    0x39, 0x0F, 0, 0, S7DP, S7G, S7DP, S7B | S7E | S7G,        /* ()*+,-./ */
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07,            /* 0-7      */
    0x7F, 0x6F, 0, 0, 0, S7D | S7G, 0, 0x53,                   /* 89:;<=>? */
    0, 0x77, 0x7C, 0x39, 0x5E, 0x79, 0x71, 0x3D,               /* @ABCDEFG: b d */
    0x76, 0x30, 0x1E, 0x75, 0x38, 0x37, 0x54, 0x3F,            /* HIJKLMNO: n   */
    0x73, 0x67, 0x50, 0x6D, 0x78, 0x3E, 0x1C, 0x7E,            /* PQRSTUVW: r t */
    0x76, 0x6E, 0x5B, 0x39, 0x64, 0x0F, 0x23, S7D,             /* XYZ[\]^_ */
};

uint8_t gm_seg7_glyph(char c)
{
    if (c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    if (c < 0x20 || c > 0x5F)
        return 0;
    return font7[c - 0x20];
}

#if !defined(GM_DISPLAY_TYPES) || ((GM_DISPLAY_TYPES >> GM_DISPLAY_TEXT) & 1u)
/* 5x7 characters, ' ' (0x20) .. '~' (0x7E): 5 columns each, bit 0 at the
 * top (the classic public-domain LCD font). */
static const uint8_t font5[95][5] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00 },  /* ' ' */
    { 0x00, 0x00, 0x5F, 0x00, 0x00 },  /* '!' */
    { 0x00, 0x07, 0x00, 0x07, 0x00 },  /* '"' */
    { 0x14, 0x7F, 0x14, 0x7F, 0x14 },  /* '#' */
    { 0x24, 0x2A, 0x7F, 0x2A, 0x12 },  /* '$' */
    { 0x23, 0x13, 0x08, 0x64, 0x62 },  /* '%' */
    { 0x36, 0x49, 0x55, 0x22, 0x50 },  /* '&' */
    { 0x00, 0x05, 0x03, 0x00, 0x00 },  /* "'" */
    { 0x00, 0x1C, 0x22, 0x41, 0x00 },  /* '(' */
    { 0x00, 0x41, 0x22, 0x1C, 0x00 },  /* ')' */
    { 0x14, 0x08, 0x3E, 0x08, 0x14 },  /* * */
    { 0x08, 0x08, 0x3E, 0x08, 0x08 },  /* '+' */
    { 0x00, 0x50, 0x30, 0x00, 0x00 },  /* ',' */
    { 0x08, 0x08, 0x08, 0x08, 0x08 },  /* '-' */
    { 0x00, 0x60, 0x60, 0x00, 0x00 },  /* '.' */
    { 0x20, 0x10, 0x08, 0x04, 0x02 },  /* / */
    { 0x3E, 0x51, 0x49, 0x45, 0x3E },  /* '0' */
    { 0x00, 0x42, 0x7F, 0x40, 0x00 },  /* '1' */
    { 0x42, 0x61, 0x51, 0x49, 0x46 },  /* '2' */
    { 0x21, 0x41, 0x45, 0x4B, 0x31 },  /* '3' */
    { 0x18, 0x14, 0x12, 0x7F, 0x10 },  /* '4' */
    { 0x27, 0x45, 0x45, 0x45, 0x39 },  /* '5' */
    { 0x3C, 0x4A, 0x49, 0x49, 0x30 },  /* '6' */
    { 0x01, 0x71, 0x09, 0x05, 0x03 },  /* '7' */
    { 0x36, 0x49, 0x49, 0x49, 0x36 },  /* '8' */
    { 0x06, 0x49, 0x49, 0x29, 0x1E },  /* '9' */
    { 0x00, 0x36, 0x36, 0x00, 0x00 },  /* ':' */
    { 0x00, 0x56, 0x36, 0x00, 0x00 },  /* ';' */
    { 0x08, 0x14, 0x22, 0x41, 0x00 },  /* '<' */
    { 0x14, 0x14, 0x14, 0x14, 0x14 },  /* '=' */
    { 0x00, 0x41, 0x22, 0x14, 0x08 },  /* '>' */
    { 0x02, 0x01, 0x51, 0x09, 0x06 },  /* '?' */
    { 0x32, 0x49, 0x79, 0x41, 0x3E },  /* '@' */
    { 0x7E, 0x11, 0x11, 0x11, 0x7E },  /* 'A' */
    { 0x7F, 0x49, 0x49, 0x49, 0x36 },  /* 'B' */
    { 0x3E, 0x41, 0x41, 0x41, 0x22 },  /* 'C' */
    { 0x7F, 0x41, 0x41, 0x22, 0x1C },  /* 'D' */
    { 0x7F, 0x49, 0x49, 0x49, 0x41 },  /* 'E' */
    { 0x7F, 0x09, 0x09, 0x09, 0x01 },  /* 'F' */
    { 0x3E, 0x41, 0x49, 0x49, 0x7A },  /* 'G' */
    { 0x7F, 0x08, 0x08, 0x08, 0x7F },  /* 'H' */
    { 0x00, 0x41, 0x7F, 0x41, 0x00 },  /* 'I' */
    { 0x20, 0x40, 0x41, 0x3F, 0x01 },  /* 'J' */
    { 0x7F, 0x08, 0x14, 0x22, 0x41 },  /* 'K' */
    { 0x7F, 0x40, 0x40, 0x40, 0x40 },  /* 'L' */
    { 0x7F, 0x02, 0x0C, 0x02, 0x7F },  /* 'M' */
    { 0x7F, 0x04, 0x08, 0x10, 0x7F },  /* 'N' */
    { 0x3E, 0x41, 0x41, 0x41, 0x3E },  /* 'O' */
    { 0x7F, 0x09, 0x09, 0x09, 0x06 },  /* 'P' */
    { 0x3E, 0x41, 0x51, 0x21, 0x5E },  /* 'Q' */
    { 0x7F, 0x09, 0x19, 0x29, 0x46 },  /* 'R' */
    { 0x46, 0x49, 0x49, 0x49, 0x31 },  /* 'S' */
    { 0x01, 0x01, 0x7F, 0x01, 0x01 },  /* 'T' */
    { 0x3F, 0x40, 0x40, 0x40, 0x3F },  /* 'U' */
    { 0x1F, 0x20, 0x40, 0x20, 0x1F },  /* 'V' */
    { 0x3F, 0x40, 0x38, 0x40, 0x3F },  /* 'W' */
    { 0x63, 0x14, 0x08, 0x14, 0x63 },  /* 'X' */
    { 0x07, 0x08, 0x70, 0x08, 0x07 },  /* 'Y' */
    { 0x61, 0x51, 0x49, 0x45, 0x43 },  /* 'Z' */
    { 0x00, 0x7F, 0x41, 0x41, 0x00 },  /* '[' */
    { 0x02, 0x04, 0x08, 0x10, 0x20 },  /* backslash */
    { 0x00, 0x41, 0x41, 0x7F, 0x00 },  /* ']' */
    { 0x04, 0x02, 0x01, 0x02, 0x04 },  /* '^' */
    { 0x40, 0x40, 0x40, 0x40, 0x40 },  /* '_' */
    { 0x00, 0x01, 0x02, 0x04, 0x00 },  /* '`' */
    { 0x20, 0x54, 0x54, 0x54, 0x78 },  /* 'a' */
    { 0x7F, 0x48, 0x44, 0x44, 0x38 },  /* 'b' */
    { 0x38, 0x44, 0x44, 0x44, 0x20 },  /* 'c' */
    { 0x38, 0x44, 0x44, 0x48, 0x7F },  /* 'd' */
    { 0x38, 0x54, 0x54, 0x54, 0x18 },  /* 'e' */
    { 0x08, 0x7E, 0x09, 0x01, 0x02 },  /* 'f' */
    { 0x0C, 0x52, 0x52, 0x52, 0x3E },  /* 'g' */
    { 0x7F, 0x08, 0x04, 0x04, 0x78 },  /* 'h' */
    { 0x00, 0x44, 0x7D, 0x40, 0x00 },  /* 'i' */
    { 0x20, 0x40, 0x44, 0x3D, 0x00 },  /* 'j' */
    { 0x7F, 0x10, 0x28, 0x44, 0x00 },  /* 'k' */
    { 0x00, 0x41, 0x7F, 0x40, 0x00 },  /* 'l' */
    { 0x7C, 0x04, 0x18, 0x04, 0x78 },  /* 'm' */
    { 0x7C, 0x08, 0x04, 0x04, 0x78 },  /* 'n' */
    { 0x38, 0x44, 0x44, 0x44, 0x38 },  /* 'o' */
    { 0x7C, 0x14, 0x14, 0x14, 0x08 },  /* 'p' */
    { 0x08, 0x14, 0x14, 0x18, 0x7C },  /* 'q' */
    { 0x7C, 0x08, 0x04, 0x04, 0x08 },  /* 'r' */
    { 0x48, 0x54, 0x54, 0x54, 0x20 },  /* 's' */
    { 0x04, 0x3F, 0x44, 0x40, 0x20 },  /* 't' */
    { 0x3C, 0x40, 0x40, 0x20, 0x7C },  /* 'u' */
    { 0x1C, 0x20, 0x40, 0x20, 0x1C },  /* 'v' */
    { 0x3C, 0x40, 0x30, 0x40, 0x3C },  /* 'w' */
    { 0x44, 0x28, 0x10, 0x28, 0x44 },  /* 'x' */
    { 0x0C, 0x50, 0x50, 0x50, 0x3C },  /* 'y' */
    { 0x44, 0x64, 0x54, 0x4C, 0x44 },  /* 'z' */
    { 0x00, 0x08, 0x36, 0x41, 0x00 },  /* '{' */
    { 0x00, 0x00, 0x7F, 0x00, 0x00 },  /* '|' */
    { 0x00, 0x41, 0x36, 0x08, 0x00 },  /* '}' */
    { 0x08, 0x04, 0x08, 0x10, 0x08 },  /* '~' */
};

const uint8_t *gm_text_glyph(char c)
{
    if (c < 0x20 || c > 0x7E)
        c = ' ';
    return font5[c - 0x20];
}
#endif

#if GM_N_DISPLAYS > 0

#ifndef GM_DISPLAY_CHARS
#define GM_DISPLAY_CHARS 16             /* the widest display (the builder allows 16) */
#endif
#define MAXC GM_DISPLAY_CHARS
/* The shows: values some display has (bit = GM_SHOWS_*): only their code is
 * built. */
#ifndef GM_DISPLAY_SHOWS
#define GM_DISPLAY_SHOWS 0x1Fu
#endif
#define SHOWS(k) ((GM_DISPLAY_SHOWS >> (k)) & 1u)
/* The types (bit = GM_DISPLAY_*), and whether a display has a segment map:
 * only what is used is built. */
#ifndef GM_DISPLAY_TYPES             /* a config generated before text displays */
#define GM_DISPLAY_TYPES ((1u << GM_DISPLAY_SEG14) | (1u << GM_DISPLAY_SEG7))
#endif
#ifndef GM_DISPLAY_MAP
#define GM_DISPLAY_MAP 1
#endif
#define HAS_SEG7 ((GM_DISPLAY_TYPES >> GM_DISPLAY_SEG7) & 1u)
#define HAS_SEG14 ((GM_DISPLAY_TYPES >> GM_DISPLAY_SEG14) & 1u)
#define HAS_TEXT ((GM_DISPLAY_TYPES >> GM_DISPLAY_TEXT) & 1u)
#define IS_SEG7(type) (HAS_SEG7 && (!HAS_SEG14 || (type) == GM_DISPLAY_SEG7))
#define IS_TEXT(type) (HAS_TEXT && (type) == GM_DISPLAY_TEXT)

extern const gm_display_t gm_displays[GM_N_DISPLAYS];   /* generated config */

/* A cell: the character (0x20..0x7E, 0 = blank) and CELL_DOT, its dot lit
 * by a '.' after it (segment displays). Segments are worked out when the
 * cell is written. */
#define CELL_DOT 0x80u
static uint8_t want[GM_N_DISPLAYS][MAXC];   /* what the device shows         */
static uint8_t host[GM_N_DISPLAYS][MAXC];   /* what a host set (FB_TEXT)     */
static uint8_t host_on[GM_N_DISPLAYS];      /* the host's text is showing    */
static uint16_t shown[GM_N_DISPLAYS][MAXC]; /* segments written to the pins (text: the cell) */
static uint8_t dirty;
static uint8_t inited;
static volatile uint32_t midi_msg;      /* seq << 24 | status << 16 | a << 8 | b */
static uint32_t midi_seen;

#define MSG_MAX 24u
#define MSG_HOLD_MS 1000u               /* a scrolling message: start and end */
#define MSG_STEP_MS 300u                /* ... and one character */
static char msg[MSG_MAX + 1];
static uint8_t msg_len;
static uint8_t msg_new;                 /* set by gm_display_message */

/* The device's own news (a message, or a program change it sent) on a
 * last_message display: for MSG_KEEP_MS plain MIDI (a CC, a note) doesn't
 * replace it, so Shift's release doesn't wipe Shift + Up's "PC 23"; and it
 * shows over a host's text (FB_TEXT) for MSG_KEEP_MS, a long message until
 * it has scrolled once, then the host's text is back. */
#define MSG_KEEP_MS 1500u
static uint8_t news_on[GM_N_DISPLAYS];
static uint32_t news_keep[GM_N_DISPLAYS];   /* plain MIDI waits until then */
static uint32_t news_over[GM_N_DISPLAYS];   /* over the host's text until then */

/* A display's scrolling text: the message, or the preset's name. */
#define SRC_MSG 0u
#define SRC_NAME 1u
typedef struct {
    uint8_t src;                        /* SRC_*                          */
    uint8_t pos;                        /* first character shown          */
    uint8_t state;                      /* 0 still, 1 scrolling, 2 holding the end */
    uint32_t t;                         /* the last step                  */
} scroll_t;
static scroll_t scr[GM_N_DISPLAYS];

#define HAS_PRESET (SHOWS(GM_SHOWS_PRESET_NUMBER) || SHOWS(GM_SHOWS_PRESET_NAME))
#if HAS_PRESET
static uint16_t preset_num;
static uint8_t preset_new;
static char pname[MSG_MAX + 1];
static uint8_t pname_len;
#endif
#if SHOWS(GM_SHOWS_CHANNEL)
static uint8_t chan, chan_new;
#endif

static uint32_t width(uint32_t d)
{
    return gm_displays[d].n_chars > MAXC ? MAXC : gm_displays[d].n_chars;
}

/* A display type's dot / point bit, and its segments for a cell. */
static uint32_t dot_bit(uint32_t type)
{
    return IS_SEG7(type) ? S7DP : SDOT;
}

static uint16_t segs(uint32_t type, uint32_t cell)
{
    char c = (char)(cell & 0x7Fu);
    uint16_t g = IS_SEG7(type) ? gm_seg7_glyph(c) : gm_seg14_glyph(c);
    return (uint16_t)(g | ((cell & CELL_DOT) ? dot_bit(type) : 0u));
}

static uint8_t cell_of(char c)
{
    return c >= 0x20 && c <= 0x7E ? (uint8_t)c : 0u;
}

void gm_display_text(uint32_t d, const char *s)
{
    if (d >= GM_N_DISPLAYS)
        return;
    uint32_t n = width(d), i = 0, type = gm_displays[d].type;
    while (*s && i <= n) {
        if (*s == '.' && !IS_TEXT(type) && i > 0 && !(segs(type, want[d][i - 1]) & dot_bit(type))) {
            want[d][i - 1] |= CELL_DOT;
        } else {
            if (i == n)
                break;
            want[d][i++] = cell_of(*s);
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

/* Copy s (at most MSG_MAX) into t; returns its length. */
static uint8_t copy_text(char *t, const char *s)
{
    uint32_t n = 0;
    while (s[n] && n < MSG_MAX) {
        t[n] = s[n];
        n++;
    }
    t[n] = 0;
    return (uint8_t)n;
}

void gm_display_message(const char *s)
{
    msg_len = copy_text(msg, s);
    msg_new = 1;
}

void gm_display_preset(uint32_t number, const char *name)
{
#if HAS_PRESET
    preset_num = (uint16_t)number;
    if (name)
        pname_len = copy_text(pname, name);
    preset_new = 1;
#else
    (void)number;
    (void)name;
#endif
}

void gm_display_channel(uint32_t channel)
{
#if SHOWS(GM_SHOWS_CHANNEL)
    if (channel != chan) {
        chan = (uint8_t)channel;
        chan_new = 1;
    }
#else
    (void)channel;
#endif
}

static int shows(uint32_t d, uint32_t what)
{
    return gm_displays[d].shows == what;
}

/* Display d's scrolling text from its pos, every character taking a place. */
static void scroll_show(uint32_t d)
{
    const char *t = msg;
    uint32_t len = msg_len, pos = scr[d].pos, n = width(d);
#if SHOWS(GM_SHOWS_PRESET_NAME)
    if (scr[d].src == SRC_NAME) {
        t = pname;
        len = pname_len;
    }
#endif
    for (uint32_t i = 0; i < n; i++)
        want[d][i] = pos + i < len ? cell_of(t[pos + i]) : 0u;
    dirty = 1;
}

static void scroll_start(uint32_t d, uint32_t src, uint32_t len, uint32_t now)
{
    scroll_t *s = &scr[d];
    s->src = (uint8_t)src;
    s->pos = 0;
    s->t = now;
    s->state = len > gm_displays[d].n_chars ? 1u : 0u;
    scroll_show(d);
}

static void scroll_step(uint32_t d, uint32_t now)
{
    scroll_t *s = &scr[d];
    uint32_t len = msg_len;
#if SHOWS(GM_SHOWS_PRESET_NAME)
    if (s->src == SRC_NAME)
        len = pname_len;
#endif
    if (!s->state || now - s->t < (s->pos == 0 || s->state == 2 ? MSG_HOLD_MS : MSG_STEP_MS))
        return;
    s->t = now;
    if (s->state == 2) {
        s->state = 0;                   /* done: back to the start */
        s->pos = 0;
    } else if (++s->pos + gm_displays[d].n_chars >= len) {
        s->state = 2;                   /* the end: hold it */
    }
    scroll_show(d);
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

/* Left text, then right text right-aligned, on display d; a dot ends the
 * left part when the two would touch. The right part is left out when it
 * doesn't fit. */
static void show_pair(uint32_t d, const char *l, uint32_t nl, const char *r, uint32_t nr)
{
    char s[MAXC * 2 + 2];
    uint32_t n = width(d), j = 0;
    for (uint32_t i = 0; i < nl && j < n; i++)
        s[j++] = l[i];
    uint32_t cells = j;
    if (nr && cells + nr <= n) {
        if (cells && cells + nr == n && !IS_TEXT(gm_displays[d].type))
            s[j++] = '.';                   /* no gap left: a dot separates them (it takes no place) */
        for (uint32_t i = cells; i + nr < n; i++)
            s[j++] = ' ';
        for (uint32_t i = 0; i < nr; i++)
            s[j++] = r[i];
    }
    s[j] = 0;
    gm_display_text(d, s);
}

#if SHOWS(GM_SHOWS_PRESET_NUMBER) || SHOWS(GM_SHOWS_CHANNEL)
/* v right-aligned on display d (its last digits, if it is too narrow). */
static void show_number(uint32_t d, uint32_t v)
{
    char r[4];
    uint32_t nr = dec(r, v), n = width(d), k = nr > n ? nr - n : 0u;
    show_pair(d, "", 0, r + k, nr - k);
}
#endif

static void news_start(uint32_t d, uint32_t len, uint32_t now)
{
    uint32_t n = gm_displays[d].n_chars, over = MSG_KEEP_MS;
    if (len > n)
        over = 2u * MSG_HOLD_MS + (len - n - 1u) * MSG_STEP_MS;  /* one scroll (scroll_step) */
    news_on[d] = 1;
    news_keep[d] = now + MSG_KEEP_MS;
    news_over[d] = now + (over > MSG_KEEP_MS ? over : MSG_KEEP_MS);
    dirty = 1;
}

/* The last MIDI message, on each last_message display: a program change
 * is news (above); a CC or note waits while there is news. */
static void format_midi(uint32_t m, uint32_t now)
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
        break;
    case 0xC0:                              /* program change */
        l[0] = 'P';
        l[1] = 'C';
        nl = 2;
        nr = dec(r, a);
        break;
    default:
        return;
    }
    int news = (st & 0xF0u) == 0xC0u;
    for (uint32_t d = 0; d < GM_N_DISPLAYS; d++) {
        if (!shows(d, GM_SHOWS_LAST_MESSAGE) || (!news && news_on[d] && (int32_t)(now - news_keep[d]) < 0))
            continue;
        scr[d].state = 0;                   /* stops a scroll */
        show_pair(d, l, nl, r, nr);
        if (news)
            news_start(d, 0, now);
    }
}

/* What a preset_* or channel display shows, now. */
static void show_value(uint32_t d, uint32_t now)
{
    (void)now;
    switch (gm_displays[d].shows) {
#if SHOWS(GM_SHOWS_PRESET_NUMBER)
    case GM_SHOWS_PRESET_NUMBER:
        show_number(d, preset_num);
        break;
#endif
#if SHOWS(GM_SHOWS_PRESET_NAME)
    case GM_SHOWS_PRESET_NAME:
        scroll_start(d, SRC_NAME, pname_len, now);
        break;
#endif
#if SHOWS(GM_SHOWS_CHANNEL)
    case GM_SHOWS_CHANNEL: {
        char r[4];
        uint32_t nr = dec(r, chan);
        if (2u + nr <= width(d))
            show_pair(d, "CH", 2, r, nr);   /* "CH 5", or "CH.5" on 3 characters */
        else
            show_number(d, chan);
        break;
    }
#endif
    default:                                /* none: blank */
        gm_display_text(d, "");
        break;
    }
}

/* The pin of segment s of character i on display dp (ns segments each). */
static gm_pin_t seg_pin(const gm_display_t *dp, uint32_t i, uint32_t s, uint32_t ns)
{
    if (GM_DISPLAY_MAP && dp->map)
        return dp->map[i * ns + s];
    gm_pin_t p = dp->chars[i];
    p.num = (uint16_t)(p.num + s);
    return p;
}

#if HAS_TEXT
/* Character i of text display dp: its 5 columns and a blank one, at
 * 6 x scale pixels a character from the row's start. */
static void text_draw(const gm_display_t *dp, uint32_t i, uint32_t cell)
{
    uint32_t sc = dp->scale ? dp->scale : 1u;
    uint8_t cols[6];
    const uint8_t *g = gm_text_glyph(cell ? (char)(cell & 0x7Fu) : ' ');
    for (uint32_t k = 0; k < 5u; k++)
        cols[k] = g[k];
    cols[5] = 0;
    gm_pin_t at = dp->chars[0];
    at.num = (uint16_t)(at.num + i * 6u * sc);
    vpin_oled_draw(at, cols, 6, sc);
}
#endif

void gm_display_poll(uint32_t now)
{
    if (!inited) {
        inited = 1;
        for (uint32_t d = 0; d < GM_N_DISPLAYS; d++)
            for (uint32_t i = 0; i < MAXC; i++)
                shown[d][i] = 0x8000u;      /* not written yet: write every segment */
        int own = dirty;                    /* text set before the first poll stays */
        for (uint32_t d = 0; d < GM_N_DISPLAYS; d++) {
            if (!shows(d, GM_SHOWS_LAST_MESSAGE))
                show_value(d, now);
            else if (!own)
                gm_display_text(d, "gemifi");
        }
    }
    for (uint32_t d = 0; d < GM_N_DISPLAYS; d++)
        if (news_on[d] && (int32_t)(now - news_over[d]) >= 0) {
            news_on[d] = 0;                 /* the host's text, if any, is back */
            dirty = 1;
        }
    uint32_t m = midi_msg;
    if (m != midi_seen) {
        midi_seen = m;
        format_midi(m, now);
    }
    int started = msg_new;              /* a message wins over MIDI in the same poll */
    if (msg_new) {
        msg_new = 0;
        for (uint32_t d = 0; d < GM_N_DISPLAYS; d++)
            if (shows(d, GM_SHOWS_LAST_MESSAGE)) {
                scroll_start(d, SRC_MSG, msg_len, now);
                news_start(d, msg_len, now);
            }
    }
#if HAS_PRESET
    if (preset_new) {
        preset_new = 0;
        for (uint32_t d = 0; d < GM_N_DISPLAYS; d++)
            if (shows(d, GM_SHOWS_PRESET_NUMBER) || shows(d, GM_SHOWS_PRESET_NAME))
                show_value(d, now);
    }
#endif
#if SHOWS(GM_SHOWS_CHANNEL)
    if (chan_new) {
        chan_new = 0;
        for (uint32_t d = 0; d < GM_N_DISPLAYS; d++)
            if (shows(d, GM_SHOWS_CHANNEL))
                show_value(d, now);
    }
#endif
    if (!started)
        for (uint32_t d = 0; d < GM_N_DISPLAYS; d++)
            scroll_step(d, now);
    if (!dirty)
        return;
    dirty = 0;
    for (uint32_t d = 0; d < GM_N_DISPLAYS; d++) {
        const gm_display_t *dp = &gm_displays[d];
        uint32_t n = width(d), ns = IS_SEG7(dp->type) ? 8u : 15u;
#if HAS_TEXT
        if (IS_TEXT(dp->type)) {
            for (uint32_t i = 0; i < n; i++) {
                uint8_t cell = host_on[d] && !news_on[d] ? host[d][i] : want[d][i];
                if (shown[d][i] == cell)
                    continue;
                shown[d][i] = cell;
                text_draw(dp, i, cell);
            }
            continue;
        }
#endif
        for (uint32_t i = 0; i < n; i++) {
            uint16_t w = segs(dp->type, host_on[d] && !news_on[d] ? host[d][i] : want[d][i]);
            uint16_t diff = (shown[d][i] & 0x8000u) ? 0x7FFFu : (uint16_t)(w ^ shown[d][i]);
            if (!diff)
                continue;
            shown[d][i] = w;
            for (uint32_t s = 0; s < ns; s++) {
                if (!(diff & (1u << s)))
                    continue;
                gm_pin_t p = seg_pin(dp, i, s, ns);
                if (p.hub != GM_HUB_NONE)
                    vpin_write(p, (w >> s) & 1u);
            }
        }
    }
}

/* FB_TEXT (sysex.md 4.6.2): from (row, col), every character taking a
 * place, clipped at the end of the row; nothing else changes. The host's
 * text covers the device's own (MIDI values), which carries on underneath
 * and shows again after gm_display_host_release, except the device's news
 * (its messages and program changes) for a moment (news_start). These
 * displays have one row. */
void gm_host_on_text(uint32_t d, uint32_t row, uint32_t col, const uint8_t *text, uint32_t n)
{
    if (d >= GM_N_DISPLAYS || row != 0)
        return;
    uint32_t w = width(d);
    if (col >= w)
        return;
    if (!host_on[d]) {
        host_on[d] = 1;
        for (uint32_t i = 0; i < MAXC; i++)
            host[d][i] = 0;
    }
    for (uint32_t i = 0; i < n && col + i < w; i++)
        host[d][col + i] = cell_of((char)text[i]);
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
void gm_display_preset(uint32_t number, const char *name) { (void)number; (void)name; }
void gm_display_channel(uint32_t channel) { (void)channel; }
void gm_display_poll(uint32_t now) { (void)now; }

#endif
