/*
 * controller.c: the generic MIDI controller runtime. See gm_controller.h
 * and core/README.md for what the components and actions do.
 *
 * Split between contexts:
 *   gm_scan_tick() (timer interrupt): key matrix, encoders not on I2C hubs,
 *     shift-register hub refresh/flush. Key changes go to the main loop
 *     through a small queue.
 *   gm_scan_frames() (the platform's DMA interrupt, same level), with a
 *     hardware scan instead: the matrices and their encoders from finished
 *     frames; the scan tick then leaves those alone.
 *   gm_poll() (main loop): everything that sends MIDI, buttons, analog
 *     inputs and pads, ramps, LEDs, I2C and TM1628 hubs, the protocol.
 * Host feedback (gm_fb_msg, from midi_in.c) also runs in the main loop: the
 * platform feeds received MIDI from its main loop.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_controller.h"
#include "gm_controls.h"
#include "gm_device.h"
#include "gm_device_config.h"
#if GM_N_SCREENS > 0
#include "gm_screen.h"                  /* display: (components/display) */
#endif
#include "gm_settings.h"
#include "gm_vpin.h"
#include "gm_hal.h"
#include "app_proto.h"
#include "params.h"
#include "gm_presets.h"
#include "gm_feedback.h"
#include "gm_mcu.h"
#include "gm_display.h"
#include "gm_din.h"
#include "gm_safety.h"


#define I2C_MS          1u
#define BOOT_LOG_MS     1500u


#ifndef GM_N_MODES
#define GM_N_MODES 0                /* a config generated before modes */
#endif
/* Modes need a button to enter them; only a mode's keys: (function keys)
 * need the key matrix, and the builder checks that (core/README.md "Modes"). */
#if GM_N_MODES > 0 && GM_N_BUTTONS == 0
#error "modes need a toggle, hold or cycle button (the builder checks this)"
#endif
#ifndef GM_LAYERS
#define GM_LAYERS 0                 /* layer twins (modes: layer:), core/README.md "Button layers" */
#endif
#if GM_LAYERS && GM_N_MODES == 0
#error "button layers belong to modes (the builder checks this)"
#endif

static uint32_t fw_ver;
static uint8_t has_sr_hubs;         /* any VPIN_SCAN_IO hub (74HC595/165): refresh in the tick */
static uint8_t has_matrices;        /* any switch-matrix hub: scan in the tick */
static uint8_t has_i2c_hubs;        /* any VPIN_BUS_IO hub (I2C): every I2C_MS */
static uint8_t has_main_hubs;       /* any VPIN_MAIN hub (TM1628, MAUDIO_PADS): vpin_poll every pass */

/* ---- MIDI out --------------------------------------------------------- */

/* write_to_flash.last_preset: when the last local input was (gm_idle_ms;
 * the keys held: keys_down). One store per message or EVENT; nothing
 * without it. */
#if GM_WRITE_LAST_PRESET
static uint32_t in_t;
#define INPUT_NOW() (in_t = hal_millis())
#else
#define INPUT_NOW() ((void)0)
#endif

uint8_t midi_chan(void)
{
    return (uint8_t)((gm_cfg.channel - 1) & 0x0F);
}

void midi3(uint8_t status, uint32_t a, uint32_t b)
{
    uint8_t m[3] = { status, (uint8_t)(a & 0x7F), (uint8_t)(b & 0x7F) };
    INPUT_NOW();
    hal_midi_send(m, 3);
    gm_din_local(m, 3);
    gm_display_midi(m[0], m[1], m[2]);
}

#if GM_N_BUTTONS > 0 || GM_N_ANALOGS > 0
void midi2(uint8_t status, uint32_t a)
{
    uint8_t m[2] = { status, (uint8_t)(a & 0x7F) };
    hal_midi_send(m, 2);
    gm_din_local(m, 2);
    gm_display_midi(m[0], m[1], 0);
}
#endif

void send_event(uint8_t type, uint32_t index, uint32_t value)
{
    INPUT_NOW();
    if (gm_cfg.events)
        app_event(type, index, value);
}

/* ---- MIDI messages (core/README.md "MIDI messages") -------------------- */

#if GM_CTL
/* A control's channel: its channel:, else midi.channel. */
uint8_t ctl_chan(const gm_ctl_t *t)
{
    return t->channel ? (uint8_t)((t->channel - 1u) & 0x0Fu) : midi_chan();
}

int32_t ctl_hi(const gm_ctl_t *t, int32_t top)
{
    return t->max == GM_CTL_TOP ? top : (int32_t)t->max;
}

/* v of 0..top as the control's min..max, rounded (min > max: inverted). */
uint32_t ctl_out(const gm_ctl_t *t, int32_t v, int32_t top)
{
    int32_t lo = t->min, d = ctl_hi(t, top) - lo;
    return (uint32_t)(lo + (v * d + (d < 0 ? -top : top) / 2) / top);
}
#endif

#if GM_MSG_KINDS
/* A kind whose values are 14-bit (a SysEx string with msb / lsb in it). */
int kind_wide(uint32_t kind, uint32_t num)
{
#if GM_N_SYSEX
    if (kind == K_SYSEX)
        return num < GM_N_SYSEX && (gm_sysex[num].flags & GM_SX_WIDE);
#endif
    (void)num;
    return kind == K_CC14 || kind == K_NRPN14 || kind == K_RPN14;
}
#endif

#if GM_N_SYSEX || GM_MSG_MMC
void sysex_out(const uint8_t *m, uint32_t n)
{
    INPUT_NOW();
    hal_midi_send(m, n);
    gm_din_local(m, n);
}
#endif

#if GM_N_SYSEX
/* SysEx string k with value v and channel ch (0..15) filled in. */
void sysex_send(uint32_t k, uint32_t v, uint8_t ch)
{
    const gm_sysex_t *s = &gm_sysex[k];
    uint8_t m[GM_SYSEX_MAX];
    uint32_t n = 0, sum = 0;
    for (uint32_t i = 0; i < s->len && n < GM_SYSEX_MAX; i++) {
        uint32_t b = s->data[i];
        switch (b) {
        case GM_SX_VALUE:
        case GM_SX_LSB:
            b = v & 0x7Fu;
            break;
        case GM_SX_MSB:
            b = (v >> 7) & 0x7Fu;
            break;
        case GM_SX_CHANNEL:
            b = ch;
            break;
        case GM_SX_SUM_START:
            sum = 0;
            continue;
        case GM_SX_SUM:
            b = (128u - (sum & 127u)) & 127u;      /* Roland */
            break;
        default:
            break;
        }
        sum += b;
        m[n++] = (uint8_t)b;
    }
    sysex_out(m, n);
}
#endif

#if GM_MSG_KINDS
/* Value v (7- or 14-bit, as kind wants) as kind on channel ch: num is the
 * CC, the (N)RPN number or the SysEx index. */
void send_value(uint32_t kind, uint8_t ch, uint32_t num, uint32_t v)
{
    uint8_t st = (uint8_t)(0xB0u | ch);
    switch (kind) {
#if GM_MSG_CC14
    case K_CC14:                    /* MSB, then the LSB on cc + 32 */
        midi3(st, num, v >> 7);
        midi3(st, num + 32u, v);
        return;
#endif
#if GM_MSG_NRPN
    case K_NRPN:
    case K_NRPN14:
    case K_RPN:
    case K_RPN14: {
        uint32_t sel = kind >= K_RPN ? 101u : 99u;     /* the number's MSB; LSB one below */
        midi3(st, sel, num >> 7);
        midi3(st, sel - 1u, num);
        if (kind == K_NRPN14 || kind == K_RPN14) {
            midi3(st, 6, v >> 7);   /* data entry MSB, LSB */
            midi3(st, 38, v);
        } else {
            midi3(st, 6, v);
        }
#if GM_NULL_RPN
        midi3(st, 101, 127);        /* the null RPN: later data entry goes nowhere */
        midi3(st, 100, 127);
#endif
        return;
    }
#endif
#if GM_N_SYSEX
    case K_SYSEX:
        if (num < GM_N_SYSEX)
            sysex_send(num, v, ch);
        return;
#endif
    default:
        midi3(st, num, v);
        return;
    }
}
#endif




volatile uint32_t scan_ticks;
volatile uint32_t gm_scan_period_us;
volatile uint32_t gm_scan_part_max[3];
uint32_t gm_poll_part_max[GM_POLL_PARTS];

__attribute__((weak)) uint32_t hal_cycles(void)
{
    return 0;
}

static void part_done(uint32_t i, uint32_t *t)
{
    uint32_t now = hal_cycles(), d = now - *t;
    if (d > gm_scan_part_max[i])
        gm_scan_part_max[i] = d;
    *t = now;
}

static void poll_part_done(uint32_t i, uint32_t *t)
{
    uint32_t now = hal_cycles(), d = now - *t;
    if (d > gm_poll_part_max[i])
        gm_poll_part_max[i] = d;
    *t = now;
}

/* ---- hardware scan (gm_controller.h gm_scan_plan_t) -------------------- */

uint8_t hw_on;
#if GM_HW_SCAN
uint32_t hw_frame_q4;
/* The most plan rows this device can have (the builder's GM_HW_SCAN_ROWS):
 * platform rings are sized for it. */
#ifdef GM_HW_SCAN_ROWS
#define HW_ROWS GM_HW_SCAN_ROWS
#else
#define HW_ROWS GM_SCAN_MAX_ROWS
#endif
static gm_scan_plan_t plan;
static uint32_t hw_prev[HW_ROWS / 2u];   /* the last frame that differed */
static uint32_t hw_mask;            /* column bits of both halfwords of a frame word */
#if GM_N_ENCODERS > 0
static uint8_t enc_fed[NE];         /* both pins on switch matrices: stepped per frame */
#endif
typedef uint32_t __attribute__((may_alias)) u32_alias;

static int on_swmatrix(gm_pin_t p)
{
    return (vpin_pin_caps(p) & VPIN_MATRIX) != 0;
}
#endif

int gm_scan_hw(void)
{
    return hw_on;
}

const gm_scan_plan_t *gm_scan_plan(void)
{
#if GM_HW_SCAN
    plan.sel_reg = 0;
    plan.in_reg = 0;
    plan.n_rows = 0;
    plan.in_mask = 0;
    plan.row_ns = GM_HW_SCAN_ROW_NS;
    plan.settle_ns = GM_HW_SCAN_SETTLE_NS;
    plan.precharge = GM_HW_SCAN_PRECHARGE;
#if GM_HAS_MATRIX
    int keys = matrix_plan(&plan, HW_ROWS);    /* key rows: plan slots 0..n_rows-1 */
    if (keys == 0)
        return 0;
    if (keys < 0)
        goto mixed;
#endif
    if (!vpin_scan_plan(&plan.sel_reg, plan.sel_word, &plan.n_rows, HW_ROWS,
                        &plan.in_reg, &plan.in_mask))
        goto mixed;
    if (!plan.n_rows || !plan.sel_reg || !plan.in_reg || (plan.in_mask >> 16)) {
        app_log(LOG_WARN, "hardware scan: nothing to scan, or columns above bit 15; ",
                "scanning in software");
        return 0;
    }
    if (plan.n_rows & 1u) {
        if (plan.n_rows >= HW_ROWS)
            goto mixed;
        plan.sel_word[plan.n_rows] = plan.sel_word[plan.n_rows - 1u];
        plan.n_rows++;
    }
    return &plan;
mixed:
    app_log(LOG_WARN, "hardware scan: rows or columns of the matrices are on different ",
            "registers, or too many rows; scanning in software");
#endif
    return 0;
}

void gm_scan_hw_started(uint32_t frame_q4)
{
#if GM_HW_SCAN
    hw_frame_q4 = frame_q4;
    hw_mask = (plan.in_mask & 0xFFFFu) * 0x10001u;
    for (uint32_t i = 0; i < HW_ROWS / 2u; i++)
        hw_prev[i] = 0xFFFFFFFFu;   /* the first frame always counts */
#if GM_N_ENCODERS > 0
    for (uint32_t i = 0; i < NE_HW; i++)
        enc_fed[i] = (uint8_t)(on_swmatrix(gm_device.encoders[i].a) &&
                               on_swmatrix(gm_device.encoders[i].b));
#endif
    vpin_scan_hw(1);
    hw_on = 1;
#else
    (void)frame_q4;
#endif
}

void gm_scan_frames(const uint16_t *raw, uint32_t n, uint32_t lost)
{
#if GM_HW_SCAN
    uint32_t words = plan.n_rows / 2u, key_rows = 0;
#if GM_HAS_MATRIX
    key_rows = gm_device.matrix ? gm_device.matrix->n_rows : 0u;
#endif
    scan_ticks += lost;
    for (uint32_t f = 0; f < n; f++, raw += plan.n_rows) {
        scan_ticks++;
        /* Word k holds rows 2k and 2k+1 (a key pair on a row-pair keybed);
         * only the column bits count (the rest of the port may be anything).
         * Nothing moved since the last frame (nearly always): nothing to do. */
        const u32_alias *w = (const u32_alias *)(const void *)raw;
        uint32_t chg = 0;
        for (uint32_t k = 0; k < words; k++) {
            uint32_t x = w[k] & hw_mask;
            if (x != hw_prev[k]) {
                chg |= 1u << k;
                hw_prev[k] = x;
            }
        }
#if GM_HAS_MATRIX
        chg |= matrix_retry(key_rows);  /* a key change didn't fit: every key row again */
#endif
        if (!chg)
            continue;
#if GM_HAS_MATRIX
        matrix_frame(raw, chg, key_rows);
#endif
        if (!(chg >> (key_rows / 2u)))
            continue;                   /* only key rows changed */
        vpin_scan_feed(raw);
#if GM_N_ENCODERS > 0
        for (uint32_t i = 0; i < NE_HW; i++)
            if (enc_fed[i])
                encoder_step(i);
#endif
    }
#else
    (void)raw;
    (void)n;
    (void)lost;
#endif
}

void gm_scan_tick(void)
{
    if (!hw_on)
        scan_ticks++;               /* hardware scan: frames count the time */
    uint32_t t = hal_cycles();
    if (has_sr_hubs)
        vpin_refresh();
#if GM_HAS_MATRIX
    if (gm_device.matrix) {
        if (!hw_on)
            matrix_scan();
        kt0_age();                  /* the frames' keys too */
    }
#endif
    part_done(0, &t);
    /* Switch matrices (panel buttons, encoders) every other tick: plenty for
     * both, and it halves their share of the scan. */
    if (has_matrices && !hw_on && (scan_ticks & 1u))
        vpin_scan_matrices();       /* before the encoders read them */
    part_done(1, &t);
#if GM_N_ENCODERS > 0
    for (uint32_t i = 0; i < NE_HW; i++) {
#if GM_HW_SCAN
        if (hw_on && enc_fed[i])
            continue;
#endif
        if (!enc_i2c[i])
            encoder_step(i);
    }
#endif
    part_done(2, &t);
    if (has_sr_hubs)
        vpin_flush();
}

/* ---- key matrix: components/key_matrix ------------------------------------ */

/* ---- modes: components/modes ----------------------------------------------- */

#if GM_N_MODES == 0
/* app_proto.c calls it whatever the device has: without modes (the modes
 * component isn't built), there's nothing to report. */
void gm_layers_report(void)
{
}
#endif

/* ---- analog inputs: components/analog ----------------------------------- */

#if GM_N_ANALOGS == 0
/* Host feedback (app_proto.c) calls it whatever the device has: without
 * analog inputs (the analog component isn't built), it does nothing. */
void gm_analog_host(uint32_t i, uint32_t value)
{
    (void)i;
    (void)value;
}
#endif

/* ---- encoders: components/encoders ---------------------------------------- */

#if GM_N_ENCODERS == 0
/* Host feedback (app_proto.c) calls it whatever the device has: without
 * encoders (the encoders component isn't built), it does nothing. */
void gm_encoder_host(uint32_t i, uint32_t value)
{
    (void)i;
    (void)value;
}
#endif

/* ---- host feedback (F1) ------------------------------------------------ */

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
#define fb_used(type, num) 0
#endif

#if GM_LED_FB
/* Bit s: some LED has source s (gm_init): most devices give few or none
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
#define leds_from(source, num, ch, value) ((void)0)
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
#endif

#if GM_WRITE_LAST_PRESET
uint32_t gm_idle_ms(void)
{
#if GM_HAS_MATRIX
    if (keys_down)
        return 0;
#endif
#if GM_N_BUTTONS > 0
    if (btn_stable)
        return 0;
#endif
    return hal_millis() - in_t;
}
#endif

/* ---- preset switch: let go of everything --------------------------------- */

void gm_release_all(void)
{
#if GM_HAS_MATRIX
    keys_off();
#endif
#if GM_N_BUTTONS > 0
    buttons_off();
#endif
#if GM_N_ANALOGS > 0
    analog_pickup_off();
#endif
#if GM_N_PADS > 0
    pads_off();
#endif
}

/* ---- init and main loop ----------------------------------------------- */

uint32_t gm_buttons_now(void)
{
    vpin_init();
    for (uint32_t i = 0; i < gm_device.n_buttons; i++)
        vpin_input(gm_device.buttons[i].pin);
    hal_delay_us(50);
    vpin_refresh();
    vpin_refresh_i2c();
    vpin_scan_matrices();
    uint32_t s = 0;
    for (uint32_t i = 0; i < gm_device.n_buttons && i < 32; i++)
        if (vpin_read(gm_device.buttons[i].pin))
            s |= 1u << i;
    return s;
}

static uint8_t safe;

void gm_init(uint32_t fw_version, int safe_mode)
{
    fw_ver = fw_version;
    safe = (uint8_t)safe_mode;
    gm_scan_period_us = gm_device.scan_us;
    uint32_t caps = vpin_caps();
    has_sr_hubs = (caps & VPIN_SCAN_IO) != 0;
    has_i2c_hubs = (caps & VPIN_BUS_IO) != 0;
    has_matrices = (caps & VPIN_MATRIX) != 0;
    has_main_hubs = (caps & VPIN_MAIN) != 0;
    vpin_init();
#if GM_N_SCREENS > 0
    gm_screen_init();
#endif
    gm_din_init();
    for (uint32_t i = 0; i < gm_device.n_buttons; i++)
        vpin_input(gm_device.buttons[i].pin);
#if GM_N_CYCLES
    cycles_init();                  /* no step yet: set once the settings are loaded (below) */
#endif
    layers_update();                /* no mode yet: every base live, every twin hidden */
    for (uint32_t i = 0; i < gm_device.n_leds; i++) {
        vpin_output(gm_device.leds[i].pin);
        if (!vpin_is_none(gm_device.leds[i].pin2))
            vpin_output(gm_device.leds[i].pin2);
        if (!vpin_is_none(gm_device.leds[i].pin3))
            vpin_output(gm_device.leds[i].pin3);
    }
#if GM_N_ENCODERS > 0
    encoders_init();
#endif
#if GM_HAS_MATRIX
    if (gm_device.matrix)
        matrix_init();
#endif
#if GM_N_MODES > 0
    modes_init();
#endif
#if GM_N_BUTTONS > 0
    buttons_init();
#endif
#if GM_N_ANALOGS > 0
    analog_init();
#endif
#if GM_N_PADS > 0
    pads_init();
#endif

    leds_init();
#if GM_LED_FB
    fb_sources = 0;
    for (uint32_t i = 0; i < GM_N_LEDS; i++)
        if (gm_led_fb[i].source < 8u)
            fb_sources |= (uint8_t)(1u << gm_led_fb[i].source);
#endif
#if GM_MCU
    gm_mcu_init(fw_version);
#endif

    gm_settings_init();
    presets_boot(safe);
    gm_safety_init(presets_boot_state());
#if GM_N_CYCLES
    cycles_start();                 /* each cycle on its first enabled step */
#endif
    layers_update();
    app_log_limit = (uint8_t)gm_cfg.log_level;
    app_proto_init(fw_version, 1);

    /* Lamp test: every LED on briefly. */
    leds_lamp(1);
    vpin_flush();
    vpin_flush_i2c();
    vpin_send_all();
#if GM_N_SCREENS > 0
    (void)gm_screen_send(0xFFFFFFFFu);
#endif
    hal_delay_us(200000);
    leds_lamp(0);
    vpin_flush();
    vpin_flush_i2c();
    vpin_send_all();
#if GM_N_SCREENS > 0
    (void)gm_screen_send(0xFFFFFFFFu);
#endif
}

/* ---- raw readings (sysex.md 4.8) ---------------------------------------- */

/* MONITOR turns the stream on for a few seconds; gm_poll sends one RAW
 * message per GM_MONITOR_INTERVAL_MS, the kinds in turn, each its next
 * chunk. The readings are what the scan last saw (it isn't stopped or
 * locked: a reading a scan tick old is fine for a tester). */
#if GM_MONITOR
#define MON_CHUNK 32u               /* values per message */
#define MON_BITS 512u               /* key-matrix contacts per message */
#define MON_KINDS 5u                /* GSX_MON_* bits */
static uint8_t mon_kinds;           /* the kinds streaming, 0 = off */
static uint8_t mon_turn;            /* the kind (bit) sending next */
static uint16_t mon_first;          /* its next chunk: first index (key matrix: row) */
static uint32_t mon_until, mon_t;
static const uint8_t mon_raw_kind[MON_KINDS] = { EV_BUTTON, EV_KEY, EV_ANALOG, EV_ENCODER, EV_PAD };

/* How many values (key matrix: rows) a kind has. */
static uint32_t mon_count(uint32_t b)
{
    switch (b) {
    case 0:
        return GM_N_OWN_BUTTONS;
#if GM_HAS_MATRIX
    case 1:
        return gm_device.matrix ? (gm_device.matrix->n_rows > FAST_ROWS ? FAST_ROWS : gm_device.matrix->n_rows) : 0u;
#endif
    case 2:
        return GM_N_ANALOGS;
    case 3:
        return NE_HW;
#if GM_N_PADS > 0
    case 4:
        return GM_N_PADS;
#endif
    default:
        return 0;
    }
}

uint32_t gm_monitor(uint32_t kinds)
{
    uint32_t have = 0;
    for (uint32_t b = 0; b < MON_KINDS; b++)
        if (mon_count(b))
            have |= 1u << b;
    kinds &= have;
    if (kinds != mon_kinds) {
        mon_turn = 0;
        mon_first = 0;
    }
    mon_kinds = (uint8_t)kinds;
    mon_until = hal_millis() + GM_MONITOR_TIMEOUT_MS;
    return kinds;
}

#if GM_N_BUTTONS > 0 || GM_HAS_MATRIX
/* Bits into 7-bit bytes, 7 per byte (bit i of byte j = bit 7j + i). Only
 * buttons and the key matrix send bits (a device with neither: unused). */
static uint32_t mon_bits(uint8_t *p, uint32_t nbits, uint32_t (*bit)(uint32_t))
{
    uint32_t n = (nbits + 6u) / 7u;
    for (uint32_t j = 0; j < n; j++)
        p[j] = 0;
    for (uint32_t b = 0; b < nbits; b++)
        if (bit(b))
            p[b / 7u] |= (uint8_t)(1u << (b % 7u));
    return n;
}
#endif

#if GM_N_BUTTONS > 0
static uint32_t mon_first_bit;      /* mon_bits callbacks: index of bit 0 */
static uint32_t mon_button(uint32_t b)
{
    return (btn_hist[0] >> (mon_first_bit + b)) & 1u;
}
#endif
#if GM_HAS_MATRIX
static uint32_t mon_cols;
static uint32_t mon_contact(uint32_t b)
{
    return (mprev[mon_first_bit + b / mon_cols] >> (b % mon_cols)) & 1u;
}
#endif

/* One kind's value i (analog, encoder, pad) as a u14. */
static uint32_t mon_value(uint32_t b, uint32_t i)
{
    switch (b) {
#if GM_N_ANALOGS > 0
    case 2:
        return a_raw[i] < 0 ? 0x3FFFu : (uint32_t)a_raw[i];
#endif
#if GM_N_ENCODERS > 0
    case 3:
        return enc_count[i] & 0x3FFFu;
#endif
#if GM_N_PADS > 0
    case 4:
        return pad_raw[i];
#endif
    default:
        return 0;
    }
}

static void monitor_poll(uint32_t now)
{
    if (!mon_kinds)
        return;
    if ((int32_t)(now - mon_until) >= 0) {
        mon_kinds = 0;              /* the host stopped renewing it */
        return;
    }
    if (now - mon_t < GM_MONITOR_INTERVAL_MS)
        return;
    mon_t = now;
    uint32_t b = mon_turn;
    while (!((mon_kinds >> b) & 1u))
        b = (b + 1u) % MON_KINDS;
    uint32_t total = mon_count(b), first = mon_first, n, cols = 1;
#if GM_HAS_MATRIX
    if (b == 1u) {
        cols = gm_device.matrix->n_cols > 32u ? 32u : gm_device.matrix->n_cols;
        if (!cols)
            cols = 1;
    }
#endif
    uint32_t chunk = b == 1u ? MON_BITS / cols : MON_CHUNK;
    n = total - first < chunk ? total - first : chunk;
    uint8_t p[4 + (MON_BITS + 6u) / 7u];
    _Static_assert(4u + 2u * MON_CHUNK <= sizeof(p), "RAW values");
    uint32_t len = 0;
    p[len++] = mon_raw_kind[b];
    if (b == 1u) {
#if GM_HAS_MATRIX
        p[len++] = (uint8_t)first;
        p[len++] = (uint8_t)n;
        p[len++] = (uint8_t)cols;
        mon_first_bit = first;
        mon_cols = cols;
        len += mon_bits(p + len, n * cols, mon_contact);
#endif
    } else {
        p[len++] = (uint8_t)(first & 0x7Fu);
        p[len++] = (uint8_t)(first >> 7);
        p[len++] = (uint8_t)n;
        if (b == 0u) {
#if GM_N_BUTTONS > 0
            mon_first_bit = first;
            len += mon_bits(p + len, n, mon_button);
#endif
        } else {
            for (uint32_t i = 0; i < n; i++) {
                uint32_t v = mon_value(b, first + i);
                p[len++] = (uint8_t)(v & 0x7Fu);
                p[len++] = (uint8_t)(v >> 7);
            }
        }
    }
    app_send_unsolicited(GSX_MSG_RAW, p, len);
    if (first + n < total) {
        mon_first = (uint16_t)(first + n);    /* the rest of this kind next time */
        return;
    }
    mon_first = 0;
    mon_turn = (uint8_t)((b + 1u) % MON_KINDS);
}
#endif

void gm_poll(void)
{
    static uint32_t t_led, t_i2c;
#if GM_N_BUTTONS > 0
    static uint32_t t_ms, t_btn;
#endif
#if GM_N_ANALOGS > 0
    static uint32_t t_analog;
#endif
#if GM_N_PADS > 0
    static uint32_t t_pad;
#endif
    static uint8_t booted;
    uint32_t now = hal_millis();
    uint32_t tc = hal_cycles();

    /* Keys and buttons first, and their MIDI to the host at once: nothing
     * below (protocol, presets, analog, LEDs, the display, the TM1628 bus)
     * stands between a key and its USB packet. Keys still go before buttons,
     * as before. Part numbers name the part, not the order. */
#if GM_HAS_MATRIX
    keys_poll();
#endif
#if GM_N_BUTTONS > 0
    if (now - t_btn >= BTN_SAMPLE_MS) {
        t_btn = now;
        buttons_sample();           /* I2C hub inputs: as refreshed on an earlier pass */
    }
    if (now - t_ms > 50u)
        t_ms = now - 1u;            /* first call, or a long stall (flash erase) */
    while (t_ms != now) {
        t_ms++;
        ramps_ms(now);
    }
#endif
    hal_midi_poll();
    poll_part_done(1, &tc);

    app_proto_poll();
#if GM_N_BUTTONS > 0
    gm_safety_poll(now, btn_stable);   /* the combos, the status LED, diag.* (gm_safety.h) */
#else
    gm_safety_poll(now, 0);
#endif
#if GM_LAYER_PARAMS
    layers_report_poll();
#endif
    gm_din_poll(now);
    presets_poll(now);
    app_log_limit = (uint8_t)gm_cfg.log_level;
    if (has_i2c_hubs && now - t_i2c >= I2C_MS) {
        t_i2c = now;
        vpin_flush_i2c();
        vpin_refresh_i2c();
    }
    poll_part_done(0, &tc);
#if GM_N_ANALOGS > 0
    if (now - t_analog >= (gm_adc_hw() ? 1u : ANALOG_MS)) {
        t_analog = now;
        analog_poll();
    }
#endif
    poll_part_done(2, &tc);
#if GM_N_PADS > 0
    if (now - t_pad >= PAD_MS) {
        t_pad = now;
        pads_scan(now);
    }
#endif
    poll_part_done(3, &tc);
#if GM_N_ENCODERS > 0
    encoders_poll(now);
#endif
#if GM_MONITOR
    monitor_poll(now);
#endif
#if GM_HAS_MATRIX
    keys_poll();                    /* keys the scan queued since the top: not a whole pass later */
#endif
    hal_midi_poll();                /* analog, pads, encoders, DIN merged in: before the LEDs too */
    poll_part_done(4, &tc);
    int led_pass = 0;
    if (now - t_led >= LED_MS) {
        t_led = now;
        leds_update(now);           /* the buttons' and pads' levels */
#if defined(GM_DISPLAY_SHOWS) && ((GM_DISPLAY_SHOWS >> GM_SHOWS_CHANNEL) & 1)
        gm_display_channel((uint32_t)gm_cfg.channel);   /* a channel display: redrawn on a change */
#endif
        gm_display_poll(now);       /* the displays' segments: written with the LEDs */
        led_pass = 1;
    } else {
        leds_apply();               /* then the LEDs, a few per pass */
    }
    poll_part_done(5, &tc);
    /* The main-loop hubs: changed TM1628 bytes, a few per pass, and not on
     * the LED update's pass (busy), so the two never add up in one pass.
     * With fade: chips, not while a round is being shown either (leds_fade).
     * The polls (MAUDIO_PADS) run on every pass. */
    int round_open = leds_round_open();
#if GM_N_SCREENS > 0
    /* The screens (display:) send what the hubs left of the bus budget. */
    uint32_t left = vpin_poll(now, led_pass || round_open);
    if (left)
        (void)gm_screen_send(left);
#else
    if (has_main_hubs)
        vpin_poll(now, led_pass || round_open);
#endif
    poll_part_done(6, &tc);
    if (!booted && now >= BOOT_LOG_MS) {
        booted = 1;
        app_log(LOG_INFO, gm_device.name, safe ? " up (safe mode: defaults)" : " up");
        app_log_hex(LOG_INFO, "fw ", fw_ver);
#if GM_N_PADS > 0
        pads_report();
#endif
    }
#if GM_HAS_MATRIX
    keys_poll();                    /* and after the LEDs and the TM1628 bus */
#endif
    hal_midi_poll();
}
