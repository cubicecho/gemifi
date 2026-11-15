/*
 * feedback.c: host feedback, F1 (core/README.md "Plain feedback"): what a
 * host sends back to the device (notes, CCs, (N)RPNs, the clock, start and
 * stop) lights LEDs, sets cc_toggle buttons and gives analog inputs (pick-up)
 * and absolute encoders the host's value. The platform's received MIDI
 * reaches it through gm_fb_msg (midi_in.c), in the main loop. Uses the
 * controls' state through gm_controls.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_controls.h"
#include "gm_device.h"
#include "gm_settings.h"
#include "gm_feedback.h"
#include "params.h"

#if GM_FEEDBACK
static uint8_t clock_n;             /* MIDI clocks since the last beat, 0..23 */

#if GM_N_BUTTONS > 0 || GM_N_ANALOGS > 0 || GM_N_ENCODERS > 0
/* The note and CC numbers some button, analog input or encoder (a twin
 * too) has, whatever its action or mode, so a host message for any other
 * number skips the walk over all of them. It may say used when the
 * component wouldn't take the message, never unused when it would. Built
 * again when the params changed (params_gen; 0 = never set, so never
 * built: gm_init's params_defaults moves it first). */
static uint32_t fb_note_used[4], fb_cc_used[4];
static uint32_t fb_used_gen;

static void fb_mark(uint32_t *map, int32_t num)
{
    if (num >= 0 && num < 128)
        map[num >> 5] |= 1u << (num & 31);
}

static int fb_used(uint32_t type, uint32_t num)
{
    if (fb_used_gen != params_gen || !params_gen) {
        for (uint32_t w = 0; w < 4; w++)
            fb_note_used[w] = fb_cc_used[w] = 0;
#if GM_N_BUTTONS > 0
        for (uint32_t i = 0; i < GM_N_BUTTONS; i++) {
            fb_mark(fb_note_used, gm_cfg.btn_number[i]);
            fb_mark(fb_cc_used, gm_cfg.btn_number[i]);
        }
#endif
#if GM_N_ANALOGS > 0
        for (uint32_t i = 0; i < GM_N_ANALOGS; i++) {
            fb_mark(fb_cc_used, gm_cfg.analog_cc[i]);
#if GM_MSG_CC14
            fb_mark(fb_cc_used, gm_cfg.analog_cc[i] + 32);     /* a cc14's LSB */
#endif
        }
#endif
#if GM_N_ENCODERS > 0
        for (uint32_t i = 0; i < GM_N_ENCODERS; i++) {
            fb_mark(fb_cc_used, gm_cfg.enc_cc[i]);
#if GM_MSG_CC14
            fb_mark(fb_cc_used, gm_cfg.enc_cc[i] + 32);
#endif
        }
#endif
#if GM_MSG_NRPN
        for (int32_t n = 98; n <= 101; n++)
            fb_mark(fb_cc_used, n);                             /* (N)RPN number */
        fb_mark(fb_cc_used, 6);                                 /* data entry */
        fb_mark(fb_cc_used, 38);
#endif
        fb_used_gen = params_gen;
    }
    const uint32_t *map = type == 0x90 ? fb_note_used : fb_cc_used;
    return num < 128 && ((map[num >> 5] >> (num & 31)) & 1u);
}
#else
#define fb_used(type, num) ((void)(type), (void)(num), 0)
#endif

#if GM_LED_FB
/* Bit s: some LED has source s (feedback_init): most devices give few or none
 * a source, so a note or CC needn't walk every LED. */
static uint8_t fb_sources;

/* The host's value for every LED with this explicit source. */
static void leds_from(uint32_t source, uint32_t num, uint32_t ch, uint32_t value)
{
    if (!((fb_sources >> source) & 1u))
        return;
    for (uint32_t i = 0; i < GM_N_LEDS; i++) {
        const gm_led_fb_t *f = &gm_led_fb[i];
        if (f->source != source)
            continue;
        if (source == GM_FB_NOTE || source == GM_FB_CC) {
            uint32_t want = f->channel ? f->channel - 1u : midi_chan();
            if (f->num != num || want != ch)
                continue;
        }
        gm_led_host(i, value);
    }
}
#else
#define leds_from(source, num, ch, value) ((void)(num), (void)(ch), (void)(value))
#endif

#if GM_N_ANALOGS > 0 || GM_N_ENCODERS > 0
/* The host's value v for the analog inputs (pick-up) and absolute encoders
 * that send kind with number num on channel ch. part: 0 a 7-bit CC; 1 data
 * entry MSB (a 7-bit (N)RPN's whole value) or a cc14's MSB; 2 an LSB. A
 * 14-bit MSB alone is taken as MSB << 7. */
static void fb_value(uint32_t kind, uint32_t ch, uint32_t num, uint32_t v, uint32_t part)
{
#if GM_N_ANALOGS > 0
    for (uint32_t i = 0; i < GM_N_ANALOGS; i++) {
#if GM_MCU
        if (gm_analog_mcu[i] != GM_MCU_NONE)
            continue;                       /* an MCU fader follows pitch bend */
#endif
#if GM_MSG_KINDS
        if (an_kind(gm_cfg.analog_mode[i]) != kind)
            continue;
#else
        if (gm_cfg.analog_mode[i] != AN_MODE_CC)
            continue;
#endif
        if ((uint32_t)gm_cfg.analog_cc[i] != num || part == 2u)
            continue;                       /* pick-up takes the top 7 bits */
#if GM_CTL
        if (AN_CH(i) != ch)
            continue;
#endif
#if GM_ANALOG_CTL
        const gm_ctl_t *t = &gm_analog_ctl[i];
#if GM_MSG_KINDS
        int32_t top = kind_wide(kind, num) ? 16383 : 127;
#else
        int32_t top = 127;
#endif
        int32_t x = top == 127 ? (int32_t)v : (int32_t)v << 7;
        int32_t lo = t->min, d = ctl_hi(t, top) - lo, q = d ? ((x - lo) * top + d / 2) / d : 0;
        q = q < 0 ? 0 : q > top ? top : q;
        gm_analog_host(i, (uint32_t)(top == 127 ? q : q >> 7));
#else
        gm_analog_host(i, v);
#endif
    }
#endif
#if GM_N_ENCODERS > 0
    for (uint32_t i = 0; i < GM_N_ENCODERS; i++) {
        int32_t mode = gm_cfg.enc_mode[i];
        if ((uint32_t)gm_cfg.enc_cc[i] != num || (mode >= ENC_CC14 ? (uint32_t)mode - 3u : K_CC) != kind)
            continue;
#if GM_CTL
        if (ENC_CH(i) != ch)
            continue;
#endif
        int32_t top = enc_top(i), span = enc_span(i, top), x = (int32_t)v;
        if (!span)
            continue;                       /* relative */
        if (top != 127) {
            if (part == 2u)
                x = ((int32_t)enc_out(i, enc_value[i], top) & 0x3F80) | x;
            else
                x <<= 7;
        } else if (part == 2u) {
            continue;
        }
        enc_at(i, x, top, span);            /* absolute: carry on from the host's value */
    }
#endif
    (void)kind;
    (void)ch;
    (void)part;
}
#endif

#if GM_MSG_NRPN
/* The (N)RPN number the host last chose, per channel; NR_RPN: an RPN. */
#define NR_RPN 0x4000u
static uint16_t nr_sel[16];
#endif

void gm_fb_msg(const uint8_t *m, uint32_t n)
{
    uint32_t st = m[0];
    if (st >= 0xF8) {
        if (st == 0xF8) {                       /* clock: 24 per quarter note */
            if (clock_n == 0)
                leds_from(GM_FB_CLOCK, 0, 0, 127);
            else if (clock_n == 6)
                leds_from(GM_FB_CLOCK, 0, 0, 0);
            clock_n = (uint8_t)(clock_n >= 23 ? 0 : clock_n + 1);
        } else if (st == 0xFA || st == 0xFB) {  /* start / continue */
            if (st == 0xFA)
                clock_n = 0;
            leds_from(GM_FB_PLAYING, 0, 0, 127);
        } else if (st == 0xFC) {                /* stop */
            leds_from(GM_FB_PLAYING, 0, 0, 0);
            leds_from(GM_FB_CLOCK, 0, 0, 0);
        }
        return;
    }
    if (n < 3)
        return;                     /* program change, channel pressure: nothing yet */
    uint32_t type = st & 0xF0u, ch = st & 0x0Fu, num = m[1], v = m[2];
    if (type == 0x80) {
        type = 0x90;
        v = 0;
    }
    if (type != 0x90 && type != 0xB0)
        return;                     /* pitch bend, poly pressure: nothing yet */
    leds_from(type == 0x90 ? GM_FB_NOTE : GM_FB_CC, num, ch, v);
#if GM_CTL
    if (!fb_used(type, num))
        return;                     /* the channel is each control's */
#else
    if (ch != midi_chan() || !fb_used(type, num))
        return;
#endif
#if GM_N_BUTTONS > 0
    for (uint32_t i = 0; i < GM_N_BUTTONS; i++) {
        if ((uint32_t)gm_cfg.btn_number[i] != num)
            continue;
#if GM_CTL
        if (BTN_CH(i) != ch)
            continue;
#endif
        int32_t act = gm_cfg.btn_action[i];
        int8_t li = gm_device.buttons[i].led;
        if (type == 0xB0 && act == ACT_CC_TOGGLE) {
            /* the host's echo is the toggle's state (and so its LED's) */
            if (v >= 64)
                btn_toggled |= 1ull << i;
            else
                btn_toggled &= ~(1ull << i);
        } else if (li >= 0 && (type == 0xB0 ? act == ACT_CC_MOMENTARY || act == ACT_CC_RAMP
                                            : act == ACT_NOTE)) {
            gm_led_host((uint32_t)li, v);
        }
    }
#endif
    if (type != 0xB0)
        return;
#if GM_MSG_NRPN
    if (num >= 98 && num <= 101) {          /* 99 / 101: the number's MSB, 98 / 100 its LSB */
        uint32_t s = nr_sel[ch];
        s = (num & 1u) ? (v << 7) | (s & 0x7Fu) : (s & 0x3F80u) | v;
        nr_sel[ch] = (uint16_t)(s | (num >= 100 ? NR_RPN : 0u));
    } else if (num == 6 || num == 38) {     /* data entry MSB, LSB */
        uint32_t s = nr_sel[ch], rpn = (s & NR_RPN) != 0, part = num == 6 ? 1u : 2u;
        fb_value(rpn ? K_RPN : K_NRPN, ch, s & 0x3FFFu, v, part);
        fb_value(rpn ? K_RPN14 : K_NRPN14, ch, s & 0x3FFFu, v, part);
    }
#endif
#if GM_MSG_CC14
    if (num < 32)
        fb_value(K_CC14, ch, num, v, 1);
    else if (num < 64)
        fb_value(K_CC14, ch, num - 32u, v, 2);
#endif
#if GM_N_ANALOGS > 0 || GM_N_ENCODERS > 0
    fb_value(K_CC, ch, num, v, 0);
#endif
}

void feedback_init(void)
{
#if GM_LED_FB
    fb_sources = 0;
    for (uint32_t i = 0; i < GM_N_LEDS; i++)
        if (gm_led_fb[i].source < 8u)
            fb_sources |= (uint8_t)(1u << gm_led_fb[i].source);
#endif
}
#endif
