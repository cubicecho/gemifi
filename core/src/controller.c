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

#ifndef GM_TM_FADE
#define GM_TM_FADE 0                /* no tm1628 hub with fade: (gm_device_config.h) */
#endif

#define BTN_SAMPLE_MS   1u
/* A press counts at the first sample that read it closed throughout (every
 * scan since the sample before: 1 ms), so a quick tap is never lost; a
 * release counts once no scan has read it closed for BTN_STABLE samples in a
 * row (16 ms), which rides out contact bounce and a light press losing
 * contact for a moment (on the Code 61, 4 ms and sometimes 10 ms counted a
 * press twice). */
#define BTN_STABLE      16u
#define ANALOG_MS       2u          /* one analog input per period */
#define LED_MS          10u
#define I2C_MS          1u
#define BEND_SEND_MS    2u
#define BOOT_LOG_MS     1500u
#define NOTE_NONE       0xFFu

#define NB GM_ARR(GM_N_BUTTONS)
#define NA GM_ARR(GM_N_ANALOGS)
#define NE GM_ARR(GM_N_ENCODERS)
#define NK GM_ARR(GM_MATRIX_KEYS)

#ifndef GM_STATUS_LED
#define GM_STATUS_LED (-1)              /* status_led: (gm_safety.h) */
#endif
#ifndef GM_N_MODES
#define GM_N_MODES 0                /* a config generated before modes */
#endif
#ifndef GM_MODE_RGB
#define GM_MODE_RGB 0               /* a cycle step with a full colour: gm_mode_rgb[] */
#endif
#ifndef GM_HW_SCAN
#define GM_HW_SCAN 0                /* a config generated before hardware_scan: */
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
/* Encoders with pins of their own; their layer twins (GM_ENC_TWINS) come
 * after them and aren't scanned. */
#define NE_HW (GM_N_ENCODERS - GM_ENC_TWINS)
#ifndef GM_ENC_ACCEL
#define GM_ENC_ACCEL 0              /* a config generated before acceleration: */
#endif

static uint32_t fw_ver;
static uint8_t has_sr_hubs;         /* any VPIN_SCAN_IO hub (74HC595/165): refresh in the tick */
static uint8_t has_matrices;        /* any switch-matrix hub: scan in the tick */
static uint8_t has_i2c_hubs;        /* any VPIN_BUS_IO hub (I2C): every I2C_MS */
static uint8_t has_main_hubs;       /* any VPIN_MAIN hub (TM1628, MAUDIO_PADS): vpin_poll every pass */

/* ---- MIDI out --------------------------------------------------------- */

/* write_to_flash.last_preset: when the last local input was, and the keys
 * held (gm_idle_ms). One store per message or EVENT; nothing without it. */
#if GM_WRITE_LAST_PRESET
static uint32_t in_t;
#define INPUT_NOW() (in_t = hal_millis())
#else
#define INPUT_NOW() ((void)0)
#endif
#if GM_WRITE_LAST_PRESET && GM_HAS_MATRIX
static uint8_t keys_down;
#define KEY_HELD(on) (keys_down = (uint8_t)((on) ? keys_down + 1u : keys_down ? keys_down - 1u : 0u))
#else
#define KEY_HELD(on) ((void)0)
#endif

static uint8_t chan(void)
{
    return (uint8_t)((gm_cfg.channel - 1) & 0x0F);
}

static void midi3(uint8_t status, uint32_t a, uint32_t b)
{
    uint8_t m[3] = { status, (uint8_t)(a & 0x7F), (uint8_t)(b & 0x7F) };
    INPUT_NOW();
    hal_midi_send(m, 3);
    gm_din_local(m, 3);
    gm_display_midi(m[0], m[1], m[2]);
}

#if GM_N_BUTTONS > 0 || GM_N_ANALOGS > 0
static void midi2(uint8_t status, uint32_t a)
{
    uint8_t m[2] = { status, (uint8_t)(a & 0x7F) };
    hal_midi_send(m, 2);
    gm_din_local(m, 2);
    gm_display_midi(m[0], m[1], 0);
}
#endif

/* unused when every kind of control has its own channel: (GM_*_CTL) */
__attribute__((unused)) static void cc(uint32_t num, uint32_t value)
{
    midi3(0xB0 | chan(), num, value);
}

static void event(uint8_t type, uint32_t index, uint32_t value)
{
    INPUT_NOW();
    if (gm_cfg.events)
        app_event(type, index, value);
}

/* ---- MIDI messages (core/README.md "MIDI messages") -------------------- */

#ifndef GM_BTN_CTL
#define GM_BTN_CTL 0                /* gm_button_ctl[]: channel:, min:, max: */
#endif
#ifndef GM_ANALOG_CTL
#define GM_ANALOG_CTL 0             /* gm_analog_ctl[] */
#endif
#ifndef GM_ENC_CTL
#define GM_ENC_CTL 0                /* gm_encoder_ctl[] */
#endif
#ifndef GM_BTN_BANK
#define GM_BTN_BANK 0               /* gm_button_bank[] */
#endif
#define GM_CTL (GM_BTN_CTL || GM_ANALOG_CTL || GM_ENC_CTL)
/* Analog inputs and encoders that can send more than a 7-bit CC. */
#define GM_MSG_KINDS (GM_MSG_WIDE || GM_N_SYSEX)

#if GM_CTL
/* A control's channel: its channel:, else midi.channel. */
static uint8_t ctl_chan(const gm_ctl_t *t)
{
    return t->channel ? (uint8_t)((t->channel - 1u) & 0x0Fu) : chan();
}

static int32_t ctl_hi(const gm_ctl_t *t, int32_t top)
{
    return t->max == GM_CTL_TOP ? top : (int32_t)t->max;
}

/* v of 0..top as the control's min..max, rounded (min > max: inverted). */
static uint32_t ctl_out(const gm_ctl_t *t, int32_t v, int32_t top)
{
    int32_t lo = t->min, d = ctl_hi(t, top) - lo;
    return (uint32_t)(lo + (v * d + (d < 0 ? -top : top) / 2) / top);
}
#endif

#if GM_MSG_KINDS
/* A kind whose values are 14-bit (a SysEx string with msb / lsb in it). */
static int kind_wide(uint32_t kind, uint32_t num)
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
static void sysex_out(const uint8_t *m, uint32_t n)
{
    INPUT_NOW();
    hal_midi_send(m, n);
    gm_din_local(m, n);
}
#endif

#if GM_N_SYSEX
/* SysEx string k with value v and channel ch (0..15) filled in. */
static void sysex_send(uint32_t k, uint32_t v, uint8_t ch)
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
static void send_value(uint32_t kind, uint8_t ch, uint32_t num, uint32_t v)
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

/* ---- key matrix (scan side) ------------------------------------------ */

#if GM_HAS_MATRIX
/* The scan tables' sizes, from the builder (core/README.md "Generated
 * code"); a config generated before them gets the most. */
#ifndef GM_MATRIX_ROWS
#define GM_MATRIX_ROWS 32           /* rows scanned: n_rows, at most 32 */
#endif
#ifndef GM_MATRIX_COL_RUNS
#define GM_MATRIX_COL_RUNS 16       /* col_run entries gm_init makes */
#endif
#ifndef GM_MATRIX_DECODER
#define GM_MATRIX_DECODER 1         /* a row on a decoder: one-store selects */
#endif
enum { K_IDLE, K_TIMING, K_ON };

typedef struct {
    uint8_t key;
    uint8_t on;
    uint16_t dt;                    /* scan ticks between the two contacts */
} kev_t;

#define KQ 32u                      /* power of two */
static volatile kev_t kq[KQ];
static volatile uint8_t kq_head, kq_tail;
static uint8_t kstate[NK];
/* The tick a key's first contact closed, low 16 bits: kt0_age() keeps a
 * timing key's age at most KT0_MAX, so ticks - kt0 never wraps. */
static uint16_t kt0[NK];
#define KT0_MAX 0x8000u
static uint8_t kt0_next;
_Static_assert(NK <= 256, "kt0_next");
#define FAST_ROWS GM_ARR(GM_MATRIX_ROWS)   /* rows scanned (at most 32) */
static uint32_t mprev[FAST_ROWS];
static uint8_t m_hubs;              /* rows or cols on a shift-register hub */
/* Fast paths (the scan runs every scan_us from an interrupt):
 * - columns all on MCU pins with an input register: a row is read as a few
 *   runs of adjacent port bits (Keystation: PB0-5, PB8, PB12 = 3 runs from
 *   one load), not pin by pin;
 * - rows all on MCU pins with an output register: one store per edge. */
#define FAST_COLS 16u
typedef struct {
    const volatile uint32_t *reg;
    uint32_t mask;                  /* run length, as low bits     */
    uint8_t src, dst;               /* port bit -> column bit      */
} col_run_t;
static col_run_t col_run[GM_ARR(GM_MATRIX_COL_RUNS)];
static uint8_t n_runs;
static uint32_t col_inv;            /* columns with GM_PIN_INVERT */
static uint8_t cols_fast;
static volatile uint32_t *row_reg[FAST_ROWS];
static uint32_t row_on;             /* level that drives row r: bit r */
static uint8_t rows_fast;
#if GM_MATRIX_DECODER
static uint8_t rows_dec;            /* every row is a decoder output */
/* Rows selected by one store each (vpin_select_word): decoder rows on a port
 * with a set/reset register. */
static volatile uint32_t *row_sel_reg[FAST_ROWS];
static uint32_t row_sel_word[FAST_ROWS];
static uint8_t rows_sel;
#else
#define rows_dec 0
#define rows_sel 0
/* The common software scan gets a loop of its own (matrix_scan_fast): rows
 * on MCU pins with an output register, every column on one input register,
 * no shift-register hubs, contacts on columns (the Keystation). gm_init
 * picks it. */
#define SCAN_FAST 1
static uint8_t scan_fast;
static const volatile uint32_t *fast_in;   /* the columns' input register */
static uint32_t fast_mask;          /* ... their bits */
static uint32_t mraw[FAST_ROWS];    /* each row's last reading & fast_mask */
#endif
#ifndef SCAN_FAST
#define SCAN_FAST 0                 /* a decoder row: matrix_scan only */
#endif
static uint8_t knote[NK], kchan[NK];
#endif

#if GM_N_MODES > 0
/* Which modes are on (core/README.md "Modes", "Cycles"): the key modes
 * (keys:, buttons:, sub-modes) share mode_cur, one at a time; a held hold
 * mode is hold_cur. With layers or cycles (GM_LAYER_PARAMS), each
 * layer-only toggle mode has its own on/off (solo_on) and each cycle its
 * current step (cyc_cur), so any of them can be on together. */
static uint8_t mode_cur = GM_MODE_NONE;     /* the active toggled key mode; GM_MODE_NONE = none */
static uint8_t hold_cur = GM_MODE_NONE;     /* the hold mode whose button is held */
#if GM_HAS_MATRIX
static uint32_t keys_mode(void);
static void mode_key(uint32_t mi, uint32_t k);
#endif
#endif
#if GM_LAYER_PARAMS
static uint64_t solo_mask;          /* layer-only toggle modes (bit = mode index), gm_init */
/* LEDs a cycle step or a layer-only toggle lights (bit = LED), gm_init:
 * mode_led() looks through the modes only for these. */
static uint32_t solo_leds[(GM_ARR(GM_N_LEDS) + 31u) / 32u];
static uint64_t solo_on;            /* ... the ones that are on */
static uint64_t modes_was;          /* modes on at the last layers_update: EVENT 8 on a change */
static uint8_t report_due;          /* gm_layers_report: the EVENT 8 replay, from gm_poll */
#if GM_N_CYCLES
static uint8_t cyc_cur[GM_N_CYCLES];   /* each cycle's current step (mode index) */
#endif
static void layers_update(void);
#else
#define layers_update() ((void)0)
#endif

/* LED levels (see led_show below): LV_BLINK | level blinks it; LV_RGB | m
 * lights the pins in mask m (an RGB colour, or a cycle step's colour);
 * LV_RGB | LV_COLOUR | m shows led_colour[] (a DAW's colour, FB_LED_RGB) on
 * an LED that can show any colour (a full-colour hub's, VPIN_RGB), else
 * mask m; LV_RGB | LV_STEP(s) | m shows step s's full colour
 * (gm_mode_rgb[s], GM_MODE_RGB) there, else mask m. */
#define LV_BLINK 0x80u
#define LV_RGB 0x40u
#define LV_COLOUR 0x20u
#define LV_STEP_SHIFT 8u            /* mode index + 1, 7 bits (the builder's MODE_RGB_MAX) */
#define LV_STEP(m) (((m) + 1u) << LV_STEP_SHIFT)
static uint16_t led_shown[GM_ARR(GM_N_LEDS)];   /* defined with leds_update() */
#if GM_HOST_FB
static uint32_t led_colour[GM_ARR(GM_N_LEDS)];  /* 0xRRGGBB, 8 bits a colour */
#endif

static volatile uint32_t ticks;
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

#if GM_HAS_MATRIX
/* A key change that didn't fit (the queue was full): matrix_row leaves its
 * bits as they were, so the next scan sees the change again; the hardware
 * scan also takes its next frame as changed (gm_scan_frames). */
static volatile uint8_t kq_retry;

/* 1 if queued, 0 if the queue is full (the main loop stalled for > 31
 * events): the caller keeps the key's old state, to try again. */
static int kq_push(uint32_t key, int on, uint32_t dt)
{
    uint8_t next = (uint8_t)((kq_head + 1u) & (KQ - 1u));
    if (next == kq_tail) {
        kq_retry = 1;
        return 0;
    }
    kq[kq_head].key = (uint8_t)key;
    kq[kq_head].on = (uint8_t)on;
    kq[kq_head].dt = (uint16_t)(dt > 0xFFFFu ? 0xFFFFu : dt);
    kq_head = next;
    return 1;
}

/* Two-contact key: whichever contact closes first starts the timer, both
 * closed = note on, both open = note off. 0 if its event didn't fit: the
 * key stays as it was (a note on then gets a later time: a little slower). */
static int key_update(uint32_t k, uint32_t c)
{
    switch (kstate[k]) {
    case K_IDLE:
        if (c == 3u) {
            if (!kq_push(k, 1, 0))  /* both at once: as fast as we can see */
                return 0;
            kstate[k] = K_ON;
        } else if (c) {
            kstate[k] = K_TIMING;
            kt0[k] = (uint16_t)ticks;
        }
        break;
    case K_TIMING:
        if (c == 3u) {
            if (!kq_push(k, 1, (uint16_t)(ticks - kt0[k])))
                return 0;
            kstate[k] = K_ON;
        } else if (!c) {
            kstate[k] = K_IDLE;
        }
        break;
    default:
        if (!c) {
            if (!kq_push(k, 0, 0))
                return 0;
            kstate[k] = K_IDLE;
        }
        break;
    }
    return 1;
}

/* One key per scan tick: a key still timing after KT0_MAX ticks stays at
 * KT0_MAX (seconds: velocity 1 long before). Each key is seen every NK
 * ticks, so its age stays far below 0x10000. */
static void kt0_age(void)
{
    uint32_t k = kt0_next;
    kt0_next = (uint8_t)(k + 1u < NK ? k + 1u : 0u);
    if (kstate[k] == K_TIMING && (uint16_t)(ticks - kt0[k]) >= KT0_MAX)
        kt0[k] = (uint16_t)(ticks - KT0_MAX);
}

/* A pin on a 74HC595/165 hub: the matrix scan must flush/refresh those
 * around each row. (A decoder row is written at once; nothing to flush.) */
static int on_sr_hub(gm_pin_t p)
{
    return (vpin_pin_caps(p) & VPIN_SCAN_IO) != 0;
}

/* Key number at matrix position pos (gm_matrix_t.key_map), or dflt without a
 * map. GM_KEY_NONE (and anything >= n_keys) is no key. */
static uint32_t key_at(const gm_matrix_t *m, uint32_t pos, uint32_t dflt)
{
    return m->key_map ? m->key_map[pos] : dflt;
}

#if GM_HW_SCAN || SCAN_FAST
/* Columns of one row from a reading of the (single) column register. */
static uint32_t cols_of(uint32_t v)
{
    uint32_t s = 0;
    for (uint32_t i = 0; i < n_runs; i++) {
        const col_run_t *k = &col_run[i];
        s |= ((v >> k->src) & k->mask) << k->dst;
    }
    return s ^ col_inv;
}
#endif

/* One row's reading s (bit c = column c closed): key changes to the queue.
 * *even carries a row-pair matrix's first row of the pair to the second. */
static void matrix_row(const gm_matrix_t *m, uint32_t r, uint32_t s, uint32_t rows, int by_rows,
                       uint32_t *even)
{
    if (by_rows) {
        /* Contacts on rows 2p and 2p+1, same column: handle the pair once
         * both rows of this pass have been read. */
        if (!(r & 1u)) {
            *even = s;
            return;
        }
        uint32_t diff = (*even ^ mprev[r - 1u]) | (s ^ mprev[r]);
        if (!diff)
            return;
        uint32_t keep = 0;              /* columns whose event didn't fit: as they were */
        uint32_t pair = r / 2u;
        for (uint32_t c = 0; c < m->n_cols; c++) {
            if (!((diff >> c) & 1u))
                continue;
            uint32_t k = key_at(m, pair * m->n_cols + c, pair + (rows / 2u) * c);
            if (k < m->n_keys && !key_update(k, ((*even >> c) & 1u) | (((s >> c) & 1u) << 1)))
                keep |= 1u << c;
        }
        mprev[r - 1u] = (*even & ~keep) | (mprev[r - 1u] & keep);
        mprev[r] = (s & ~keep) | (mprev[r] & keep);
        return;
    }
    uint32_t diff = s ^ mprev[r];
    if (!diff)
        return;
    uint32_t keep = 0;                  /* bits whose event didn't fit: as they were */
    if (m->kind == GM_MATRIX_KIND_VELOCITY) {
        for (uint32_t p = 0; 2u * p + 1u < m->n_cols; p++) {
            if (!((diff >> (2u * p)) & 3u))
                continue;
            uint32_t k = key_at(m, r * (m->n_cols / 2u) + p, r + rows * p);
            if (k < m->n_keys && !key_update(k, (s >> (2u * p)) & 3u))
                keep |= 3u << (2u * p);
        }
    } else {
        for (uint32_t c = 0; c < m->n_cols; c++) {
            if (!((diff >> c) & 1u))
                continue;
            uint32_t k = key_at(m, r * m->n_cols + c, r + rows * c);
            if (k < m->n_keys && !kq_push(k, (s >> c) & 1u, 0))
                keep |= 1u << c;
        }
    }
    mprev[r] = (s & ~keep) | (mprev[r] & keep);
}

#if SCAN_FAST
/* The pins see what matrix_scan does to them: row on, settle, read the
 * columns, row off. A row that reads as last time (nearly always) is then
 * done: its columns are only sorted out of the port bits, and handed to
 * matrix_row, when something changed. */
static void matrix_scan_fast(const gm_matrix_t *m)
{
    const volatile uint32_t *in = fast_in;
    uint32_t mask = fast_mask, on = row_on, settle = m->settle_us, rows = m->n_rows;
    for (uint32_t r = 0; r < rows; r++, on >>= 1) {
        volatile uint32_t *reg = row_reg[r];
        *reg = on & 1u;
        if (settle)
            hal_delay_us(settle);
        uint32_t raw = *in & mask;
        *reg = (on & 1u) ^ 1u;
        if (raw != mraw[r]) {
            mraw[r] = raw;
            matrix_row(m, r, cols_of(raw), rows, 0, 0);
        }
    }
}

/* gm_init, once the rows and columns are set up. (Not inlined: smaller
 * that way, as gm_init is large.) */
__attribute__((noinline)) static void scan_fast_init(const gm_matrix_t *m)
{
    scan_fast = rows_fast && cols_fast && !m_hubs && n_runs &&
                !(m->kind == GM_MATRIX_KIND_VELOCITY && m->contacts == GM_MATRIX_CONTACTS_ROWS);
    fast_in = col_run[0].reg;
    fast_mask = 0;
    for (uint32_t i = 0; i < n_runs; i++) {
        if (col_run[i].reg != fast_in)
            scan_fast = 0;
        fast_mask |= col_run[i].mask << col_run[i].src;
    }
    for (uint32_t r = 0; r < FAST_ROWS; r++)
        mraw[r] = ~0u;              /* never a reading: the first scan sorts out every row */
}
#endif

static void matrix_scan(void)
{
    const gm_matrix_t *m = gm_device.matrix;
#if SCAN_FAST
    if (scan_fast) {
        matrix_scan_fast(m);
        return;
    }
#endif
    uint32_t rows = m->n_rows > FAST_ROWS ? FAST_ROWS : m->n_rows;
    int by_rows = m->kind == GM_MATRIX_KIND_VELOCITY && m->contacts == GM_MATRIX_CONTACTS_ROWS;
    uint32_t even = 0;              /* by_rows: this pass's reading of row r - 1 */
    for (uint32_t r = 0; r < rows; r++) {
#if GM_MATRIX_DECODER
        if (rows_sel)
            *row_sel_reg[r] = row_sel_word[r];
        else
#endif
        if (rows_fast)
            *row_reg[r] = (row_on >> r) & 1u;
        else
            vpin_write(m->rows[r], 1);
        if (m_hubs)
            vpin_flush();
        if (m->settle_us)
            hal_delay_us(m->settle_us);
        if (m_hubs)
            vpin_refresh();
        uint32_t s = 0;
        if (cols_fast) {
            const volatile uint32_t *last = 0;
            uint32_t v = 0;
            for (uint32_t i = 0; i < n_runs; i++) {
                const col_run_t *k = &col_run[i];
                if (k->reg != last) {
                    last = k->reg;
                    v = *last;
                }
                s |= ((v >> k->src) & k->mask) << k->dst;
            }
            s ^= col_inv;
        } else {
            for (uint32_t c = 0; c < m->n_cols && c < 32; c++)
                if (vpin_read(m->cols[c]))
                    s |= 1u << c;
        }
        if (rows_fast)
            *row_reg[r] = ((row_on >> r) & 1u) ^ 1u;
        else if (!rows_dec)
            vpin_write(m->rows[r], 0);  /* a decoder row: the next select replaces it */
        matrix_row(m, r, s, rows, by_rows, &even);
    }
    if (m_hubs)
        vpin_flush();
}
#endif

/* ---- encoders (scan side, or main loop for I2C hubs) ------------------ */

#if GM_N_ENCODERS > 0
/* Scan state: only the scanned encoders (NE_HW) have it, not their twins. */
static uint8_t enc_ab[GM_ARR(NE_HW)];
static int8_t enc_acc[GM_ARR(NE_HW)];
/* Detents, free running: only encoder_step writes it (the scan interrupt,
 * or the main loop for I2C hubs, never both for one encoder), and the main
 * loop takes the turns as count - enc_seen. No read-modify-write from both
 * sides, so a detent counted between the main loop's load and store isn't
 * lost (the M0 has no LDREX; a halfword load or store is atomic). Twins
 * aren't scanned: only the scanned encoders have counts. */
static volatile uint16_t enc_count[GM_ARR(NE_HW)];
static uint16_t enc_seen[GM_ARR(NE_HW)];
static uint8_t enc_i2c[GM_ARR(NE_HW)];
static int16_t enc_value[NE];
/* One-load reads of A and B (vpin_in_reg), electrical levels; 0 = vpin_read. */
static const volatile uint32_t *enc_ra[GM_ARR(NE_HW)], *enc_rb[GM_ARR(NE_HW)];
static uint32_t enc_ma[GM_ARR(NE_HW)], enc_mb[GM_ARR(NE_HW)];
#endif
#if GM_ENC_TWINS
static uint8_t enc_live[NE];        /* per scanned encoder: the index its turns go to (layers_update) */
#define ENC_LIVE(i) enc_live[i]
#else
#define ENC_LIVE(i) (i)
#endif

#if GM_N_ENCODERS > 0
/* An encoder's channel; min / max (channel:, min:, max:). */
#if GM_ENC_CTL
#define ENC_CH(i) ctl_chan(&gm_encoder_ctl[i])
#else
#define ENC_CH(i) chan()
#endif

/* The top of encoder i's values: 127, or 16383 in a 14-bit mode. */
static int32_t enc_top(uint32_t i)
{
#if GM_MSG_KINDS
    int32_t mode = gm_cfg.enc_mode[i];
    if (mode >= ENC_CC14 && kind_wide((uint32_t)mode - 3u, (uint32_t)gm_cfg.enc_cc[i]))
        return 16383;
#else
    (void)i;
#endif
    return 127;
}

/* In an absolute mode, enc_value is the position 0..span along min..max
 * (0..top without them); 0: a relative mode. */
static int32_t enc_span(uint32_t i, int32_t top)
{
    int32_t mode = gm_cfg.enc_mode[i];
    if (mode != ENC_ABSOLUTE && mode < ENC_CC14)
        return 0;
#if GM_ENC_CTL
    int32_t s = ctl_hi(&gm_encoder_ctl[i], top) - gm_encoder_ctl[i].min;
    return s < 0 ? -s : s;
#else
    return top;
#endif
}

/* The value position p sends, and the position of a value v. */
static uint32_t enc_out(uint32_t i, int32_t p, int32_t top)
{
#if GM_ENC_CTL
    const gm_ctl_t *t = &gm_encoder_ctl[i];
    return (uint32_t)(ctl_hi(t, top) >= t->min ? t->min + p : t->min - p);
#else
    (void)i;
    (void)top;
    return (uint32_t)p;
#endif
}

static void enc_at(uint32_t i, int32_t v, int32_t top, int32_t span)
{
#if GM_ENC_CTL
    const gm_ctl_t *t = &gm_encoder_ctl[i];
    v = ctl_hi(t, top) >= t->min ? v - t->min : t->min - v;
#else
    (void)top;
#endif
    enc_value[i] = (int16_t)(v < 0 ? 0 : v > span ? span : v);
}
#endif

/* FB_VALUE: value 0..16383 over the encoder's range. */
void gm_encoder_host(uint32_t i, uint32_t value)
{
#if GM_N_ENCODERS > 0
    if (i >= GM_N_ENCODERS)
        return;
    int32_t span = enc_span(i, enc_top(i));
    if (span == 127)
        enc_value[i] = (int16_t)(value >> 7);
    else if (span)
        enc_value[i] = (int16_t)((int32_t)value * span / 16383);
#else
    (void)i;
    (void)value;
#endif
}

#if GM_N_ENCODERS > 0

static void encoder_step(uint32_t i)
{
    static const int8_t tab[16] = { 0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0 };
    const gm_encoder_t *e = &gm_device.encoders[i];
    uint32_t ab;
    if (enc_ra[i] && enc_rb[i]) {
        uint32_t a = ((*enc_ra[i] & enc_ma[i]) != 0) ^ ((e->a.flags & GM_PIN_INVERT) != 0);
        uint32_t b = ((*enc_rb[i] & enc_mb[i]) != 0) ^ ((e->b.flags & GM_PIN_INVERT) != 0);
        ab = a << 1 | b;
    } else {
        ab = (uint32_t)(vpin_read(e->a) << 1) | (uint32_t)vpin_read(e->b);
    }
    enc_acc[i] = (int8_t)(enc_acc[i] + tab[(enc_ab[i] << 2) | ab]);
    enc_ab[i] = (uint8_t)ab;
    int32_t spd = e->steps_per_detent ? e->steps_per_detent : 4;
    if (enc_acc[i] >= spd) {
        enc_acc[i] = 0;
        enc_count[i]++;
    } else if (enc_acc[i] <= -spd) {
        enc_acc[i] = 0;
        enc_count[i]--;
    }
}
#endif

/* ---- hardware scan (gm_controller.h gm_scan_plan_t) -------------------- */

static uint8_t hw_on;               /* the platform scans; frames come to gm_scan_frames */
#if GM_HW_SCAN
static uint32_t hw_frame_q4;        /* frame period, 1/16 us */
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
    const gm_matrix_t *m = gm_device.matrix;
    if (m) {
        /* Key rows are plan slots 0..n_rows-1, in order (gm_scan_frames). */
        if (!rows_sel || !cols_fast || m_hubs || m->n_rows > HW_ROWS) {
            app_log(LOG_WARN, "hardware scan: key matrix rows need one-store selects and ",
                    "columns on one input register; scanning in software");
            return 0;
        }
#if GM_MATRIX_DECODER
        for (uint32_t r = 0; r < m->n_rows; r++) {
            if (plan.sel_reg && row_sel_reg[r] != plan.sel_reg)
                goto mixed;
            plan.sel_reg = row_sel_reg[r];
            plan.sel_word[r] = row_sel_word[r];
        }
#endif
        for (uint32_t i = 0; i < n_runs; i++) {
            if (plan.in_reg && col_run[i].reg != plan.in_reg)
                goto mixed;
            plan.in_reg = col_run[i].reg;
            plan.in_mask |= col_run[i].mask << col_run[i].src;
        }
        plan.n_rows = m->n_rows;
    }
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
    const gm_matrix_t *m = gm_device.matrix;
    int by_rows = m && m->kind == GM_MATRIX_KIND_VELOCITY && m->contacts == GM_MATRIX_CONTACTS_ROWS;
    key_rows = m ? m->n_rows : 0u;
#endif
    ticks += lost;
    for (uint32_t f = 0; f < n; f++, raw += plan.n_rows) {
        ticks++;
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
        if (kq_retry) {                 /* a key change didn't fit: every key row again */
            kq_retry = 0;
            chg |= (1u << ((key_rows + 1u) / 2u)) - 1u;
        }
#endif
        if (!chg)
            continue;
#if GM_HAS_MATRIX
        for (uint32_t k = 0; m && 2u * k < key_rows; k++) {
            if (!((chg >> k) & 1u))
                continue;
            uint32_t even = 0;
            matrix_row(m, 2u * k, cols_of(raw[2u * k]), key_rows, by_rows, &even);
            if (2u * k + 1u < key_rows)
                matrix_row(m, 2u * k + 1u, cols_of(raw[2u * k + 1u]), key_rows, by_rows, &even);
        }
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
        ticks++;                    /* hardware scan: frames count the time */
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
    if (has_matrices && !hw_on && (ticks & 1u))
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

/* ---- velocity ---------------------------------------------------------- */

#if GM_HAS_MATRIX
/* log2(x) in 1/256 units (piecewise linear: within 0.09 of exact). */
static int32_t log2_q8(uint32_t x)
{
    if (!x)
        return 0;
    int32_t msb = 31 - __builtin_clz(x);
    uint32_t frac = msb >= 8 ? (x >> (msb - 8)) & 0xFFu : (x << (8 - msb)) & 0xFFu;
    return msb * 256 + (int32_t)frac;
}

/* log2_q8 of the key times, for the fast / slow they were taken of (the
 * settings change rarely; a note on shouldn't pay for two logs). 0 / 0 to
 * start, which is never a key: slow > fast. */
static int32_t vk_fast, vk_slow;
static int16_t vk_lf, vk_ls;

/* Stock-like curve: 127 at vel_fast, 1 at vel_slow, linear in log(time)
 * between (about -18 velocity per doubling with the defaults). */
static uint32_t velocity(uint16_t dt_ticks)
{
    if (gm_cfg.vel_curve == CURVE_FIXED || gm_device.matrix->kind != GM_MATRIX_KIND_VELOCITY)
        return (uint32_t)gm_cfg.vel_fixed;
    /* In microseconds: a hardware scan's ticks (frames) are a few tens of
     * us, finer than the 0.1 ms steps of vel_fast / vel_slow. The tick
     * period is in 1/16 us. (dt * q4) >> 4 as two products that fit 32
     * bits, dt <= 0xFFFF (kq_push saturates it) and q4 < 2^20 (the scan
     * period is at most 0xFFFF us; the platforms slow it to 5 ms at most,
     * frames are tens of us): no divide. A time of 2^28 or more (the whole
     * product over 32 bits) saturates there, as it always has. */
    uint32_t q4 = gm_scan_period_us * 16u;
#if GM_HW_SCAN
    if (hw_on)
        q4 = hw_frame_q4;
#endif
    if (q4 > 0xFFFFFu)
        q4 = 0xFFFFFu;
    uint32_t t = dt_ticks * (q4 >> 4) + ((dt_ticks * (q4 & 15u)) >> 4);
    if (t > 0x0FFFFFFFu)
        t = 0x0FFFFFFFu;
    int32_t fast = gm_cfg.vel_fast * 100, slow = gm_cfg.vel_slow * 100;
    if (slow <= fast)
        slow = fast + 1;
    int32_t v;
    if ((int32_t)t <= fast) {
        v = 127;
    } else if ((int32_t)t >= slow) {
        v = 1;
    } else {
        if (fast != vk_fast || slow != vk_slow) {
            vk_fast = fast;
            vk_slow = slow;
            vk_lf = (int16_t)log2_q8((uint32_t)fast);
            vk_ls = (int16_t)log2_q8((uint32_t)slow);
            if (vk_ls <= vk_lf)         /* fast and slow in one log step (above ~32 ms): no divide by 0 */
                vk_ls = (int16_t)(vk_lf + 1);
        }
        int32_t lf = vk_lf, ls = vk_ls;
        v = 127 - 126 * (log2_q8(t) - lf) / (ls - lf);
    }
    if (gm_cfg.vel_curve == CURVE_SOFT)
        v = 127 - (127 - v) * (127 - v) / 127;
    else if (gm_cfg.vel_curve == CURVE_HARD)
        v = v * v / 127;
    return (uint32_t)(v < 1 ? 1 : v > 127 ? 127 : v);
}

static int32_t key_note(uint32_t k)
{
    return (int32_t)gm_device.matrix->first_note + (int32_t)k +
           12 * gm_cfg.octave + gm_cfg.transpose;
}

static void keys_poll(void)
{
    while (kq_tail != kq_head) {
        kev_t e = kq[kq_tail];
        kq_tail = (uint8_t)((kq_tail + 1u) & (KQ - 1u));
        KEY_HELD(e.on);
        if (e.on) {
#if GM_N_MODES > 0
            uint32_t mi = keys_mode();
            if (mi != GM_MODE_NONE) {
                knote[e.key] = NOTE_NONE;   /* a function key: no note, its release sends nothing */
                mode_key(mi, e.key);
                continue;
            }
#endif
            int32_t n = key_note(e.key);
            uint32_t v = velocity(e.dt);
            if (n < 0 || n > 127) {
                knote[e.key] = NOTE_NONE;
                continue;
            }
            knote[e.key] = (uint8_t)n;
            kchan[e.key] = chan();
            midi3(0x90 | kchan[e.key], (uint32_t)n, v);
            event(EV_KEY, e.key, v);
        } else {
            if (knote[e.key] != NOTE_NONE)
                midi3(0x80 | kchan[e.key], knote[e.key], 0);
            knote[e.key] = NOTE_NONE;
            event(EV_KEY, e.key, 0);
        }
    }
}

/* Note Off for every sounding key; their releases then send nothing. */
static void keys_off(void)
{
    if (!gm_device.matrix)
        return;
    for (uint32_t k = 0; k < NK; k++) {
        if (knote[k] == NOTE_NONE)
            continue;
        midi3(0x80 | kchan[k], knote[k], 0);
        knote[k] = NOTE_NONE;
    }
}
#endif

/* ---- buttons ----------------------------------------------------------- */

#if GM_N_BUTTONS > 0
static uint64_t btn_stable, btn_toggled;   /* bit = button (at most 64) */
/* Held, and the press went to this button: a layer twin and its base read
 * the same pin, but only the live one takes the press (and its release). */
static uint64_t btn_held;
#if GM_LAYERS
static uint64_t btn_live = ~0ull;   /* layers_update() */
#define BTN_LIVE_ALL btn_live
#else
#define BTN_LIVE_ALL (~0ull)
#endif
/* A reading of every button, one bit each, in the narrowest type that holds
 * GM_N_BUTTONS (64-bit shifts are library calls on a Cortex-M0). */
#if GM_N_BUTTONS <= 8
typedef uint8_t btn_bits_t;
#elif GM_N_BUTTONS <= 16
typedef uint16_t btn_bits_t;
#elif GM_N_BUTTONS <= 32
typedef uint32_t btn_bits_t;
#else
typedef uint64_t btn_bits_t;
#endif
static btn_bits_t btn_hist[BTN_STABLE - 1u];  /* the samples before the latest */
static uint64_t btn_ignore;         /* held through a preset switch: release is silent */
static int16_t ramp_q8[NB];         /* CC ramp position, 0..127 << 8 (fits 16 bits) */
static int16_t ramp_sent[NB];
static uint8_t btn_note[NB], btn_chan[NB];
static int32_t bend, bend_target, bend_sent = 0x7FFFFFFF;
static int32_t program;

/* A button's channel and its CC values (min:, max:; 0 and 127 by default). */
#if GM_BTN_CTL
#define BTN_CH(i) ctl_chan(&gm_button_ctl[i])
#define BTN_OUT(i, v, top) ctl_out(&gm_button_ctl[i], (int32_t)(v), (top))
#define BTN_CC(i, num, v) midi3((uint8_t)(0xB0u | BTN_CH(i)), (num), BTN_OUT(i, v, 127))
#else
#define BTN_CH(i) chan()
#define BTN_OUT(i, v, top) ((uint32_t)(v))
#define BTN_CC(i, num, v) cc((num), (v))
#endif

/* The Program Change of a program action, after the button's bank: if it
 * has one. */
static void program_send(uint32_t i, uint32_t pc)
{
    uint8_t ch = BTN_CH(i);
#if GM_BTN_BANK
    uint32_t bank = gm_button_bank[i];
    if (bank != GM_BANK_NONE) {
        midi3((uint8_t)(0xB0u | ch), 0, bank >> 7);
        midi3((uint8_t)(0xB0u | ch), 32, bank);
    }
#else
    (void)i;
#endif
    midi2((uint8_t)(0xC0u | ch), pc);
}

#if GM_MSG_PANIC
/* Panic: let go of every note gemifi holds (keys, buttons, pads, CCs held
 * on, pitch bend), as a preset switch does, then All Sound Off, All Notes
 * Off and Reset All Controllers on every channel. */
static void panic(void)
{
    gm_release_all();
    for (uint32_t c = 0; c < 16u; c++) {
        midi3((uint8_t)(0xB0u | c), 120, 0);
        midi3((uint8_t)(0xB0u | c), 123, 0);
        midi3((uint8_t)(0xB0u | c), 121, 0);
    }
}
#endif

/* Pressed in every scan since the last sample; *any: in any of them. A
 * button on a switch matrix is scanned many times a sample (each frame of
 * a hardware scan), others are read once. */
static btn_bits_t read_buttons(btn_bits_t *any)
{
    btn_bits_t s = 0, a = 0;
    vpin_span_take();
    for (uint32_t i = 0; i < GM_N_BUTTONS && i < 64; i++) {
        int seen;
        if (vpin_read_span(gm_device.buttons[i].pin, &seen))
            s |= (btn_bits_t)1 << i;
        if (seen)
            a |= (btn_bits_t)1 << i;
    }
    *any = a;
    return s;
}

static int held_with_action(int32_t act, uint32_t except)
{
    for (uint32_t i = 0; i < GM_N_BUTTONS; i++)
        if (i != except && ((btn_held >> i) & 1u) && gm_cfg.btn_action[i] == act)
            return 1;
    return 0;
}

static void bend_retarget(void)
{
    int dn = held_with_action(ACT_BEND_DOWN, 0xFFFFu);
    int up = held_with_action(ACT_BEND_UP, 0xFFFFu);
    bend_target = (dn && !up) ? -8192 : (up && !dn) ? 8191 : 0;
}

static void shift(int32_t *v, int32_t step, int32_t lim, uint16_t id, int reset)
{
    int32_t n = reset ? 0 : *v + step;
    if (n < -lim)
        n = -lim;
    if (n > lim)
        n = lim;
    if (n != *v) {
        *v = n;
        app_param_changed(id, 0);
    }
    /* "OCT +1", "TR -12", "OCT 0" on the display, also at a limit */
    char t[8];
    uint32_t j = 0, a = (uint32_t)(n < 0 ? -n : n);
    const char *l = id == P_OCTAVE ? "OCT " : id == P_TRANSPOSE ? "TR " : "PAD ";
    while (*l)
        t[j++] = *l++;
    if (n)
        t[j++] = n < 0 ? '-' : '+';
    if (a >= 10u)
        t[j++] = (char)('0' + a / 10u % 10u);
    t[j++] = (char)('0' + a % 10u);
    t[j] = 0;
    gm_display_message(t);
}

static void button_changed(uint32_t i, int down)
{
    int32_t act = gm_cfg.btn_action[i];
    uint32_t num = (uint32_t)gm_cfg.btn_number[i];
    event(EV_BUTTON, i, (uint32_t)down);
    app_log(LOG_DEBUG, gm_device.buttons[i].name, down ? " down" : " up");
    switch (act) {
    case ACT_CC_MOMENTARY:
        BTN_CC(i, num, down ? 127 : 0);
        break;
    case ACT_CC_TOGGLE:
        if (down) {
            btn_toggled ^= 1ull << i;
            BTN_CC(i, num, ((btn_toggled >> i) & 1u) ? 127 : 0);
        }
        break;
    case ACT_NOTE:
        if (down) {
            btn_note[i] = (uint8_t)num;
            btn_chan[i] = BTN_CH(i);
            midi3(0x90 | btn_chan[i], num, (uint32_t)gm_cfg.vel_fixed);
        } else if (btn_note[i] != NOTE_NONE) {
            midi3(0x80 | btn_chan[i], btn_note[i], 0);
            btn_note[i] = NOTE_NONE;
        }
        break;
    case ACT_OCTAVE_DOWN:
    case ACT_OCTAVE_UP:
        if (down) {
            int other = held_with_action(act == ACT_OCTAVE_UP ? ACT_OCTAVE_DOWN : ACT_OCTAVE_UP, i);
            shift(&gm_cfg.octave, act == ACT_OCTAVE_UP ? 1 : -1, 4, P_OCTAVE, other);
        }
        break;
    case ACT_TRANSPOSE_DOWN:
    case ACT_TRANSPOSE_UP:
        if (down) {
            int other = held_with_action(act == ACT_TRANSPOSE_UP ? ACT_TRANSPOSE_DOWN : ACT_TRANSPOSE_UP, i);
            shift(&gm_cfg.transpose, act == ACT_TRANSPOSE_UP ? 1 : -1, 24, P_TRANSPOSE, other);
        }
        break;
    case ACT_BEND_DOWN:
    case ACT_BEND_UP:
        bend_retarget();
        break;
#if GM_N_PADS > 0
    case ACT_PAD_OCTAVE_DOWN:
    case ACT_PAD_OCTAVE_UP:
        if (down) {     /* both held: back to 0, as the octave buttons */
            int other = held_with_action(act == ACT_PAD_OCTAVE_UP ? ACT_PAD_OCTAVE_DOWN : ACT_PAD_OCTAVE_UP, i);
            shift(&gm_cfg.pad_octave, act == ACT_PAD_OCTAVE_UP ? 1 : -1, 4, P_PAD_OCTAVE, other);
        }
        break;
#endif
    case ACT_PROGRAM_NEXT:
    case ACT_PROGRAM_PREV:
        if (down) {
            program = (program + (act == ACT_PROGRAM_NEXT ? 1 : 127)) & 127;
            program_send(i, (uint32_t)program);
        }
        break;
#if GM_MSG_PROGRAM
    case ACT_PROGRAM:
        if (down) {
            program = (int32_t)(num & 127u);    /* program next / previous go on from it */
            program_send(i, num);
        }
        break;
#endif
#if GM_MSG_PANIC
    case ACT_PANIC:
        if (down)
            panic();
        break;
#endif
#if GM_MSG_MMC
    case ACT_MMC:
        if (down) {
            const uint8_t m[6] = { 0xF0, 0x7F, GM_MMC_DEVICE, 0x06, (uint8_t)(num & 0x7Fu), 0xF7 };
            sysex_out(m, 6);
        }
        break;
#endif
#if GM_N_SYSEX
    case ACT_SYSEX:
        /* press: the value max: (top by default), release: min: (0), if the
         * string has a value at all */
        if (num < GM_N_SYSEX && (down || (gm_sysex[num].flags & GM_SX_HAS_VALUE))) {
            uint32_t top = (gm_sysex[num].flags & GM_SX_WIDE) ? 16383u : 127u;
            sysex_send(num, BTN_OUT(i, down ? top : 0u, (int32_t)top), BTN_CH(i));
        }
        break;
#endif
    case ACT_PRESET_NEXT:
    case ACT_PRESET_PREV:
        if (down)
            presets_step(act == ACT_PRESET_NEXT ? 1 : -1);
        break;
    case ACT_PRESET_SELECT:
        if (down && num >= 1 && num <= GM_N_PRESETS)
            presets_select(num);
        break;
#if GM_MCU
    case ACT_MCU:
        gm_mcu_note(num, down);
        break;
#endif
    default:                        /* ACT_NONE, ACT_CC_RAMP (in ramps_ms) */
        break;
    }
}

/* ---- modes (core/README.md "Modes") ------------------------------------ */

#if GM_N_MODES > 0
#define SHOW_STEP_MS 200u           /* preset number blink: one step */
#define MODE_BLINK_MS 250u          /* a blinking mode LED: half period */
static uint8_t show_n;              /* slot being shown on the LEDs, 0 = none */
static uint8_t show_mode;           /* the mode whose LEDs show it */
static uint32_t show_t0;
static uint64_t btn_in_mode;        /* pressed as a mode's function button: its release is silent */

#if GM_HAS_MATRIX
/* The mode the keys go to: a held hold mode with keys, else the toggled
 * one if it has keys; GM_MODE_NONE = the keys play. */
static uint32_t keys_mode(void)
{
    if (hold_cur != GM_MODE_NONE && gm_modes[hold_cur].n_keys)
        return hold_cur;
    if (mode_cur != GM_MODE_NONE && gm_modes[mode_cur].n_keys)
        return mode_cur;
    return GM_MODE_NONE;
}

/* Mode md is about to start: if it takes the keys and no mode had them,
 * the held keys get their Note Off first (their releases send nothing). */
static void keys_to_mode(const gm_mode_t *md)
{
    if (md->n_keys && keys_mode() == GM_MODE_NONE)
        keys_off();
}
#else
#define keys_to_mode(md) ((void)(md))   /* no key matrix: no mode has keys (the builder checks) */
#endif

/* "CH <n>" on the display. */
static void show_channel(void)
{
    char t[6] = "CH ";
    uint32_t c = (uint32_t)gm_cfg.channel, j = 3;
    if (c >= 10u)
        t[j++] = (char)('0' + c / 10u);
    t[j++] = (char)('0' + c % 10u);
    t[j] = 0;
    gm_display_message(t);
}

static int binds_channel(const gm_mode_key_t *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        if (b[i].op == GM_MK_CHANNEL || b[i].op == GM_MK_CHANNEL_UP || b[i].op == GM_MK_CHANNEL_DOWN)
            return 1;
    return 0;
}

#if GM_LAYER_PARAMS
/* How strongly root mode m's layer holds, 0 = off. When layers on at once
 * have twins of the same component, the strongest wins: the held hold
 * mode, then the toggled key mode, then cycle steps, then layer-only
 * toggles, each in mode order (core/README.md "Cycles"). */
static uint32_t layer_rank(uint32_t m)
{
    if (hold_cur == m)
        return 255u;
    if (mode_cur != GM_MODE_NONE && gm_modes[mode_cur].root == m)
        return 254u;
#if GM_N_CYCLES
    uint32_t g = gm_modes[m].cycle;
    if (g && cyc_cur[g - 1u] == m)
        return 200u - m;            /* GM_N_MODES <= 64 */
#endif
    if ((solo_on >> m) & 1u)
        return 128u - m;
    return 0;
}

#if GM_N_CYCLES
/* The step of cycle g (1..) after mode from: the next enabled one, round
 * the group; GM_MODE_NONE: the first enabled one (power-on). Every step
 * disabled: the first step. */
static uint32_t cycle_after(uint32_t g, uint32_t from)
{
    uint32_t first = 0;
    while (first < GM_N_MODES - 1u && gm_modes[first].cycle != g)
        first++;
    uint32_t s = from == GM_MODE_NONE ? first + GM_N_MODES - 1u : from;
    for (uint32_t k = 1; k <= GM_N_MODES; k++) {
        uint32_t m = (s + k) % GM_N_MODES;
        if (gm_modes[m].cycle == g && gm_cfg.layer_enabled[m])
            return m;
    }
    return first;
}

/* The cycle button: the next enabled step's layer, its name on the
 * display, its colour on the cycle's LED (mode_led), EVENT 8 for the old
 * step (0) and the new one (1) (layers_update). */
static void cycle_press(uint32_t g)
{
    uint32_t m = cycle_after(g, cyc_cur[g - 1u]);
    cyc_cur[g - 1u] = (uint8_t)m;
    layers_update();
    if (gm_modes[m].name)
        gm_display_message(gm_modes[m].name);
}
#endif
#endif

/* Another hold mode whose button is still held (two held at once: letting
 * go of one goes back to the other), or GM_MODE_NONE. */
static uint8_t hold_still_held(uint32_t except)
{
    for (uint32_t m = 0; m < GM_N_MODES; m++)
        if (m != except && gm_modes[m].hold && gm_modes[m].toggle < GM_N_BUTTONS &&
            ((btn_held >> gm_modes[m].toggle) & 1u)
#if GM_LAYER_PARAMS
            && gm_cfg.layer_enabled[m]
#endif
            )
            return (uint8_t)m;
    return GM_MODE_NONE;
}

/* The toggle or hold button of a root mode. Toggle: enters the mode (held
 * keys get their Note Off first, if it has keys), or leaves it from the
 * mode or any of its sub-modes. Hold: the mode is active while the button
 * is down. Its own action never runs, except a modifier's (a hold mode
 * with only a layer, like Shift): that switches its layer and returns 0,
 * so the button's action runs too. Returns 0 if button i is neither.
 * A twin of a toggle: { button, with } button toggles the mode as well.
 * A cycle's button (on each of its steps) moves it to its next step. A
 * disabled mode (layer.enabled 0) can't be entered: its button is as if
 * it weren't there (it can still be left). */
static int mode_button(uint32_t i, int down)
{
#if GM_LAYERS
    const gm_button_t *bt = &gm_device.buttons[i];
    uint32_t base = bt->layer ? bt->base : i;
#else
    uint32_t base = i;
#endif
    for (uint32_t m = 0; m < GM_N_MODES; m++) {
        const gm_mode_t *md = &gm_modes[m];
        if (md->toggle != base || (base != i && !md->with))
            continue;
#if GM_LAYER_PARAMS
        if (!md->cycle && !gm_cfg.layer_enabled[m] && !layer_rank(m) && !(btn_in_mode & (1ull << i)))
            continue;
#endif
        if (md->with && !md->hold) {
            /* toggle: { button, with }: only with that button held; a plain
             * press is the button's own (its release too). */
            uint64_t bit = 1ull << i;
            if (down ? !((btn_held >> (md->with - 1u)) & 1u) : !(btn_in_mode & bit))
                return 0;
            if (down)
                btn_in_mode |= bit;
            else
                btn_in_mode &= ~bit;
        }
        if (!md->modifier)
            event(EV_BUTTON, i, (uint32_t)down);    /* a modifier's: button_changed */
#if GM_N_CYCLES
        if (md->cycle) {
            if (down)
                cycle_press(md->cycle);
            return 1;
        }
#endif
        if (md->hold) {
            if (down) {
                keys_to_mode(md);
                hold_cur = (uint8_t)m;
                if (binds_channel(md->buttons, md->n_buttons) || binds_channel(md->keys, md->n_keys))
                    show_channel();
            } else if (hold_cur == m) {
                hold_cur = hold_still_held(m);
            }
            layers_update();
            app_log(LOG_DEBUG, gm_device.buttons[i].name, down ? ": mode on" : ": mode off");
            return !md->modifier;
        }
        if (!down)
            return 1;
#if GM_LAYER_PARAMS
        if ((solo_mask >> m) & 1u) {
            solo_on ^= 1ull << m;           /* a layer of its own: the others stay */
            layers_update();
            app_log(LOG_DEBUG, gm_device.buttons[i].name, ((solo_on >> m) & 1u) ? ": layer on" : ": layer off");
            return 1;
        }
#endif
        if (mode_cur != GM_MODE_NONE && gm_modes[mode_cur].root == m) {
            mode_cur = GM_MODE_NONE;
        } else {
            keys_to_mode(md);
            mode_cur = (uint8_t)m;
        }
        layers_update();
        app_log(LOG_DEBUG, gm_device.buttons[i].name, mode_cur == GM_MODE_NONE ? ": mode off" : ": mode on");
        return 1;
    }
    return 0;
}

/* A device-side change of a per-preset param: the host hears of it, and the
 * preset is now dirty (like a host edit: COMMIT keeps it, a switch drops it). */
static void set_live(int32_t *v, int32_t x, uint16_t id)
{
    if (*v == x)
        return;
    *v = x;
    app_param_changed(id, 0);
    presets_host_set(id, 0);
}

/* Run a binding of mode mi (a key's or a button's), then a sub-mode goes
 * back to its parent. */
static void mode_run(uint32_t mi, const gm_mode_key_t *b)
{
    const gm_mode_t *m = &gm_modes[mi];
    int up = 1;
    switch (b->op) {
    case GM_MK_MODE:
        mode_cur = b->arg;
        return;
    case GM_MK_PRESET:
    case GM_MK_PRESET_NEXT:
    case GM_MK_PRESET_PREV:
        if (b->op == GM_MK_PRESET)
            presets_select(b->arg);
        else
            presets_step(b->op == GM_MK_PRESET_NEXT ? 1 : -1);
        show_n = (uint8_t)presets_active();
        show_mode = (uint8_t)mi;
        show_t0 = hal_millis();
        break;
    case GM_MK_OCTAVE_DOWN:
        up = -1;
        /* fall through */
    case GM_MK_OCTAVE_UP:
    case GM_MK_OCTAVE_RESET:
        shift(&gm_cfg.octave, up, 4, P_OCTAVE, b->op == GM_MK_OCTAVE_RESET);
        break;
    case GM_MK_TRANSPOSE_DOWN:
        up = -1;
        /* fall through */
    case GM_MK_TRANSPOSE_UP:
    case GM_MK_TRANSPOSE_RESET:
        shift(&gm_cfg.transpose, up, 24, P_TRANSPOSE, b->op == GM_MK_TRANSPOSE_RESET);
        break;
    case GM_MK_CHANNEL:
    case GM_MK_CHANNEL_UP:
    case GM_MK_CHANNEL_DOWN: {
        int32_t c = b->op == GM_MK_CHANNEL ? b->arg
                  : b->op == GM_MK_CHANNEL_UP ? gm_cfg.channel % 16 + 1
                  : (gm_cfg.channel + 14) % 16 + 1;
        set_live(&gm_cfg.channel, c, P_CHANNEL);
        show_channel();
        break;
    }
    case GM_MK_CURVE:
        set_live(&gm_cfg.vel_curve, b->arg, P_VEL_CURVE);
        break;
    default:
        break;
    }
    if (m->parent != GM_MODE_NONE && mode_cur == mi)
        mode_cur = m->parent;
}

#if GM_HAS_MATRIX
/* A key pressed in mode mi: run its binding (unbound keys do nothing). */
static void mode_key(uint32_t mi, uint32_t k)
{
    const gm_mode_t *m = &gm_modes[mi];
    for (uint32_t i = 0; i < m->n_keys; i++)
        if (m->keys[i].key == k) {
            mode_run(mi, &m->keys[i]);
            return;
        }
}
#endif

/* Button i pressed or released while a mode is active: a button the mode
 * binds runs that instead of its own action (the held hold mode first, then
 * the toggled one), and its release is silent, even after the mode ends.
 * Returns 1 if the mode took the change. */
static int mode_btn(uint32_t i, int down)
{
    uint64_t bit = 1ull << i;
    if (!down) {
        if (!(btn_in_mode & bit))
            return 0;
        btn_in_mode &= ~bit;
        event(EV_BUTTON, i, 0);
        return 1;
    }
    btn_in_mode &= ~bit;
    const uint8_t cand[2] = { hold_cur, mode_cur };
    for (uint32_t c = 0; c < 2; c++) {
        uint32_t mi = cand[c];
        if (mi == GM_MODE_NONE)
            continue;
        const gm_mode_t *m = &gm_modes[mi];
        for (uint32_t k = 0; k < m->n_buttons; k++)
            if (m->buttons[k].key == i) {
                btn_in_mode |= bit;
                event(EV_BUTTON, i, 1);
                app_log(LOG_DEBUG, gm_device.buttons[i].name, ": mode action");
                mode_run(mi, &m->buttons[k]);
                return 1;
            }
    }
    return 0;
}

/* What LED li shows instead of level: after a preset switch from a mode,
 * its count LED blinks the slot number, then it and the done LED flash
 * once; while a mode is active (or its hold button held), its LED is lit
 * (or blinks). */
/* An int8_t LED reference (-1 = none) is LED li. Compared unsigned: a
 * device has up to 255 LEDs, and (int8_t)li is negative from 128 on. */
#define LED_IS(ref, li) ((ref) >= 0 && (uint32_t)(ref) == (li))

static uint32_t mode_led(uint32_t li, uint32_t level, uint32_t now)
{
    if (show_n) {
        const gm_mode_t *m = &gm_modes[show_mode];
        uint32_t s = (now - show_t0) / SHOW_STEP_MS, n2 = 2u * show_n;
        int is_count = LED_IS(m->blink_count, li), is_done = LED_IS(m->blink_done, li);
        if (s > n2 + 2u)
            show_n = 0;             /* done: a dark step, N blinks, a 2-step flash */
        else if (is_count || is_done)
            return s > n2 ? 1u : is_count && (s & 1u);
    }
    if (hold_cur != GM_MODE_NONE && LED_IS(gm_modes[hold_cur].led, li))
        return gm_modes[hold_cur].led_blink ? (now / MODE_BLINK_MS) & 1u : 1u;
    if (mode_cur != GM_MODE_NONE && LED_IS(gm_modes[mode_cur].led, li))
        return gm_modes[mode_cur].led_blink ? (now / MODE_BLINK_MS) & 1u : 1u;
#if GM_LAYER_PARAMS
    if (!((solo_leds[li >> 5] >> (li & 31u)) & 1u))
        return level;
    for (uint32_t m = 0; m < GM_N_MODES; m++) {     /* cycle steps and layer-only toggles */
        const gm_mode_t *md = &gm_modes[m];
        if (!LED_IS(md->led, li) || !(md->cycle || ((solo_mask >> m) & 1u)) || !layer_rank(m))
            continue;
        if (md->colour) {
#if GM_MODE_RGB
            if (gm_mode_rgb[m] && m < 127u)
                return LV_RGB | LV_STEP(m) | (md->colour & 7u);  /* its full colour, where it shows */
#endif
            return LV_RGB | (md->colour & 7u);       /* the step's colour */
        }
        return md->led_blink ? (now / MODE_BLINK_MS) & 1u : 1u;
    }
#endif
    return level;
}
#define MODE_LED(li, level, now) mode_led(li, level, now)
#endif

static void buttons_sample(void)
{
    /* A press counts once a sample saw it in every scan (a contact that
     * only flickered closed, or noise, doesn't press); any scan that saw it
     * closed keeps it held for BTN_STABLE more samples. */
    btn_bits_t seen;
    btn_bits_t now = read_buttons(&seen);
    btn_bits_t recent = seen;       /* seen pressed in any of the last BTN_STABLE samples */
    for (uint32_t k = 0; k < BTN_STABLE - 1u; k++)
        recent |= btn_hist[k];
    for (uint32_t k = BTN_STABLE - 1u; k > 1u; k--)
        btn_hist[k - 1u] = btn_hist[k - 2u];
    btn_hist[0] = seen;
    uint64_t s = now;
    uint64_t st = s | (btn_stable & recent);
    if (st != btn_stable) {
        uint64_t diff = st ^ btn_stable;
        btn_stable = st;
        s = st;
        /* Who is live as the pass starts: a press that switches a layer
         * isn't taken again by the twin it makes live. */
        uint64_t live = BTN_LIVE_ALL;
        for (uint32_t i = 0; i < GM_N_BUTTONS && i < 64; i++) {
            if (!((diff >> i) & 1u))
                continue;
            int down = (s >> i) & 1u;
            if (gm_safety_button(i, down))
                continue;                     /* an unlock / factory-reset combo's (gm_safety.h) */
            if (down ? !((live >> i) & 1u) : !((btn_held >> i) & 1u))
                continue;                     /* the press is the live twin's (or a hidden one's release) */
            if (down)
                btn_held |= 1ull << i;
            else
                btn_held &= ~(1ull << i);
            if ((btn_ignore >> i) & 1u) {
                btn_ignore &= ~(1ull << i);   /* released after a preset switch */
#if GM_N_MODES > 0
                if (hold_cur != GM_MODE_NONE && gm_modes[hold_cur].toggle == i) {
                    hold_cur = hold_still_held(hold_cur);  /* a hold mode still ends with its button */
                    layers_update();
                }
                btn_in_mode &= ~(1ull << i);
#endif
                continue;
            }
#if GM_N_MODES > 0
            if (mode_button(i, down))
                continue;
            if (mode_btn(i, down))
                continue;
#endif
            button_changed(i, down);
        }
    }
}

/* Called once per elapsed millisecond. */
/* The buttons set to cc_ramp, worked out again when a param changes: the
 * millisecond loop looks at those only. */
static btn_bits_t ramp_mask;
static uint32_t ramp_gen;

static void ramps_ms(uint32_t now)
{
    if (ramp_gen != params_gen || !params_gen) {
        ramp_gen = params_gen;
        ramp_mask = 0;
        for (uint32_t i = 0; i < GM_N_BUTTONS; i++)
            if (gm_cfg.btn_action[i] == ACT_CC_RAMP)
                ramp_mask |= (btn_bits_t)1u << i;
    }
    for (btn_bits_t m = ramp_mask; m; m &= m - 1u) {
        uint32_t i = (uint32_t)__builtin_ctzll(m);
        int32_t target = ((btn_held >> i) & 1u) ? (127 << 8) : 0;
        int32_t step = gm_cfg.ramp_time > 0 ? (127 << 8) / gm_cfg.ramp_time : (127 << 8);
        if (step < 1)
            step = 1;
        if (ramp_q8[i] < target)
            ramp_q8[i] = (int16_t)(ramp_q8[i] + step > target ? target : ramp_q8[i] + step);
        else if (ramp_q8[i] > target)
            ramp_q8[i] = (int16_t)(ramp_q8[i] - step < target ? target : ramp_q8[i] - step);
        int16_t v = (int16_t)(ramp_q8[i] >> 8);
        if (v != ramp_sent[i]) {
            ramp_sent[i] = v;
            BTN_CC(i, (uint32_t)gm_cfg.btn_number[i], (uint32_t)v);
        }
    }
    int32_t step = gm_cfg.bend_glide > 0 ? 8192 / gm_cfg.bend_glide : 16384;
    if (step < 1)
        step = 1;
    if (bend < bend_target)
        bend = bend + step > bend_target ? bend_target : bend + step;
    else if (bend > bend_target)
        bend = bend - step < bend_target ? bend_target : bend - step;
    static uint32_t t_bend;
    if (bend != bend_sent && (now - t_bend >= BEND_SEND_MS || bend == bend_target)) {
        t_bend = now;
        bend_sent = bend;
        uint32_t v = (uint32_t)(bend + 8192);
        midi3(0xE0 | chan(), v & 0x7F, v >> 7);
    }
}
#endif

/* ---- analog inputs ---------------------------------------------------- */

#if GM_N_ANALOGS == 0
/* Host feedback (app_proto.c) calls it whatever the device has. */
void gm_analog_host(uint32_t i, uint32_t value)
{
    (void)i;
    (void)value;
}
#endif

#if GM_N_ANALOGS > 0
static int32_t a_filt[NA];          /* 0..4095 << 2 */
static int16_t a_sent[NA];          /* last 7-bit value, -1 = none yet */
static uint8_t a_next;
/* analog.mode pitch bend: the reading at power-up is the centre (a sprung
 * wheel rests there); BEND_DEAD either side of it is no bend. Readings
 * past BEND_END of either end are full bend. A change under BEND_STEP is
 * noise, except to the centre or an end. The dead zone and the noise step
 * are raw ADC counts, so they stay the same size on a wheel whose
 * adc_min..adc_max is only a small part of 0..4095 (the Code 61's: ~940). */
#define BEND_DEAD 16                /* raw counts */
#define BEND_END 8                  /* 0..4095 after adc_min / adc_max */
#define BEND_STEP 6                 /* raw counts */
static int16_t a_center[NA];        /* 0..4095 */
static int16_t a_bend[NA];          /* last pitch bend sent, 0..16383 */
/* BEND_DEAD in 0..4095 units and BEND_STEP in bend units for each input's
 * adc_min..adc_max (bend_scale, at init: the range is in the device table). */
static uint16_t a_dead[NA], a_step[NA];
#if GM_ANALOG_PICKUP
/* Pick-up: after the host sets a value the knob isn't at, the knob stays
 * silent until it reaches or crosses that value. */
static int8_t a_pick[NA];           /* host value to reach, -1 = in control */
static int8_t a_side[NA];           /* side of a_pick the knob was on: -1 / 1 */
#endif
#if GM_ANALOG_TOUCH
/* touch: movement: EVENT touch 1 just before a movement's first value EVENT,
 * 0 once the value has been still for TOUCH_STILL_MS. */
#define TOUCH_STILL_MS 300u
static uint8_t a_touched[NA];
static uint32_t a_moved[NA];        /* ms of the last value EVENT */

static void touch_poll(void)
{
    uint32_t now = hal_millis();
    for (uint32_t i = 0; i < GM_N_ANALOGS; i++)
        if (a_touched[i] && now - a_moved[i] >= TOUCH_STILL_MS) {
            a_touched[i] = 0;
            event(EV_TOUCH, i, 0);
        }
}
#endif

/* An input's channel and output range (channel:, min:, max:). */
#if GM_ANALOG_CTL
#define AN_CH(i) ctl_chan(&gm_analog_ctl[i])
#define AN_OUT(i, v, top) ctl_out(&gm_analog_ctl[i], (int32_t)(v), (top))
#else
#define AN_CH(i) chan()
#define AN_OUT(i, v, top) ((uint32_t)(v))
#endif
#if GM_MSG_KINDS
/* What analog.mode sends: K_CC for cc, 0xFF for pitch bend and pressure. */
static uint32_t an_kind(int32_t mode)
{
    return mode >= AN_MODE_CC14 ? (uint32_t)mode - 2u : mode == AN_MODE_CC ? K_CC : 0xFFu;
}

/* pos (0..4095) in 14 bits, the last BEND_END of each end at the end. */
static int32_t an_wide(int32_t pos)
{
    return pos <= BEND_END ? 0 : pos >= 4095 - BEND_END ? 16383 : pos * 16383 / 4095;
}
#endif

#if GM_ANALOG_PICKUP
/* 1: input i at q (0..127) hasn't picked up the host's value yet, so it
 * sends nothing. Reaching or crossing it picks it up. */
static int not_picked(uint32_t i, int32_t q)
{
    if (a_pick[i] < 0)
        return 0;
    int side = q < a_pick[i] ? -1 : q > a_pick[i] ? 1 : 0;
    if (side == a_side[i])
        return 1;
    a_pick[i] = -1;
    return 0;
}
#endif

/* A value EVENT (pos 0..4095), after a touch EVENT if this starts a movement. */
static void analog_event(uint32_t i, int32_t pos)
{
#if GM_ANALOG_TOUCH
    if (gm_analog_touch[i] && gm_cfg.events) {
        a_moved[i] = hal_millis();
        if (!a_touched[i]) {
            a_touched[i] = 1;
            event(EV_TOUCH, i, 1);
        }
    }
#endif
    event(EV_ANALOG, i, (uint32_t)pos << 2);
}

void gm_analog_host(uint32_t i, uint32_t value)
{
#if GM_ANALOG_PICKUP
    if (!gm_analog_pickup[i])
        return;
    int32_t at = a_sent[i];
    a_pick[i] = -1;
    if (at >= 0 && at != (int32_t)value) {
        a_pick[i] = (int8_t)value;
        a_side[i] = (int8_t)(at < (int32_t)value ? -1 : 1);
    }
#else
    (void)i;
    (void)value;
#endif
}

/* a_dead / a_step for input i. A step over 16384 acts as 16384 (no change
 * of bend is that big); the dead zone fits (at most 16 * 4095). */
static void bend_scale(uint32_t i)
{
    const gm_analog_t *a = &gm_device.analogs[i];
    int32_t range = a->raw_max > a->raw_min ? a->raw_max - a->raw_min : a->raw_min - a->raw_max;
    if (range == 0)
        range = 4095;
    int32_t step = BEND_STEP * 16384 / range;
    a_dead[i] = (uint16_t)(BEND_DEAD * 4095 / range);
    a_step[i] = (uint16_t)(step > 16384 ? 16384 : step);
}

/* Pitch bend from analog input i at pos (0..4095): centre to each end is
 * one half of the 14-bit range. */
static void analog_bend(uint32_t i, int32_t pos)
{
    int32_t dead = a_dead[i];                         /* in 0..4095 units */
    int32_t step = a_step[i];                         /* in bend units */
    int32_t c = a_center[i], b = 8192;
    if (pos > c + dead) {
        int32_t span = 4095 - BEND_END - c - dead;
        b = span > 0 ? 8192 + (pos - c - dead) * 8191 / span : 16383;
    } else if (pos < c - dead) {
        int32_t span = c - dead - BEND_END;
        b = span > 0 ? 8192 - (c - dead - pos) * 8192 / span : 0;
    }
    b = b < 0 ? 0 : b > 16383 ? 16383 : b;
    int32_t last = a_bend[i], d = b > last ? b - last : last - b;
    if (d == 0 || (d < step && b != 8192 && b != 0 && b != 16383))
        return;
    a_bend[i] = (int16_t)b;
    midi3(0xE0 | chan(), (uint32_t)b & 0x7Fu, (uint32_t)b >> 7);
    analog_event(i, pos);
}

#if GM_MSG_KINDS
/* A 14-bit kind (cc14, nrpn14, rpn14, a SysEx with msb / lsb): pos as
 * 0..16383, sent when it moves by half pitch bend's noise step (3 raw
 * counts) or reaches an end. a_bend holds the last value, a_sent its top 7
 * bits (pick-up, the host's value). */
static void analog_wide(uint32_t i, int32_t pos, uint32_t kind, uint32_t num)
{
    int32_t b = an_wide(pos);
    int32_t last = a_bend[i], d = b > last ? b - last : last - b;
    if (d == 0 || (d < (a_step[i] >> 1) && b != 0 && b != 16383))
        return;
    a_bend[i] = (int16_t)b;
    a_sent[i] = (int16_t)(b >> 7);
#if GM_ANALOG_PICKUP
    if (not_picked(i, b >> 7))
        return;
#endif
    send_value(kind, AN_CH(i), num, AN_OUT(i, b, 16383));
    analog_event(i, pos);
}
#endif

#if GM_MONITOR
static int16_t a_raw[NA];           /* the last reading, -1 = none: RAW (sysex.md 4.8) */
#define A_RAW(i, r) (a_raw[i] = (int16_t)(r))
#else
#define A_RAW(i, r) ((void)0)
#endif

static void analog_read(uint32_t i)
{
    const gm_analog_t *a = &gm_device.analogs[i];
    int32_t raw = vpin_analog(a->pin);
    A_RAW(i, raw < 0 ? -1 : raw);
    if (raw < 0)
        return;
    int32_t lo = a->raw_min, hi = a->raw_max, v;
    if (hi == lo) {
        v = raw;
    } else {
        v = (raw - lo) * 4095 / (hi - lo);   /* hi < lo: reversed */
        v = v < 0 ? 0 : v > 4095 ? 4095 : v;
    }
    /* An XY pad reads nothing untouched; a touch jumps to where it is, and
     * the first touch sends (it isn't the power-up reading). */
    int jump = vpin_analog_jump(a->pin);
    int32_t mode = gm_cfg.analog_mode[i];
#if GM_MSG_KINDS
    uint32_t kind = an_kind(mode), num = (uint32_t)gm_cfg.analog_cc[i];
    int wide = kind != 0xFFu && kind_wide(kind, num);
#endif
    if (a_sent[i] < 0 && !jump) {
        a_filt[i] = v << 2;                 /* first reading: no message */
        a_sent[i] = (int16_t)(v >> 5);
        a_center[i] = (int16_t)v;
        a_bend[i] = 8192;
#if GM_MSG_KINDS
        if (wide)
            a_bend[i] = (int16_t)an_wide(v);
#endif
        return;
    }
    if (jump)
        a_filt[i] = v << 2;
    else
        a_filt[i] += v - (a_filt[i] >> 2); /* 4-sample moving average */
    int32_t pos = a_filt[i] >> 2;           /* 0..4095 */
    if (mode == AN_MODE_BEND) {
        analog_bend(i, pos);
        return;
    }
#if GM_MSG_KINDS
    if (wide) {
        analog_wide(i, pos, kind, num);
        return;
    }
#endif
    int32_t center = a_sent[i] * 32 + 16;
    if (pos - center > 24 || center - pos > 24 ||
        (pos >= 4088 && a_sent[i] != 127) || (pos <= 8 && a_sent[i] != 0)) {
        int16_t q = (int16_t)(pos >> 5);
        if (q != a_sent[i]) {
            a_sent[i] = q;
#if GM_ANALOG_PICKUP
            if (not_picked(i, q))
                return;
#endif
#if GM_MCU
            if (gm_analog_mcu[i] != GM_MCU_NONE)
                gm_mcu_fader(gm_analog_mcu[i], (uint32_t)pos << 2);
            else
#endif
            if (mode == AN_MODE_PRESSURE)
                midi2((uint8_t)(0xD0u | AN_CH(i)), AN_OUT(i, q, 127));   /* channel pressure (aftertouch) */
            else
#if GM_MSG_KINDS
                send_value(kind, AN_CH(i), num, AN_OUT(i, q, 127));
#elif GM_ANALOG_CTL
                midi3((uint8_t)(0xB0u | AN_CH(i)), (uint32_t)gm_cfg.analog_cc[i], AN_OUT(i, q, 127));
#else
                cc((uint32_t)gm_cfg.analog_cc[i], (uint32_t)q);
#endif
            analog_event(i, pos);
        }
    }
}

/* With the hardware analog scan: every input, since the readings are
 * already there. Without it: one input per ANALOG_MS in turn (each takes an
 * ADC conversion and a mux settle), and every pitch-bend and pressure input
 * each time as well, so a wheel or a pressure strip follows quickly however
 * many inputs there are. */
static void analog_poll(void)
{
#if GM_ANALOG_TOUCH
    touch_poll();
#endif
    if (gm_adc_hw()) {
        for (uint32_t i = 0; i < GM_N_ANALOGS; i++)
            analog_read(i);
        return;
    }
    uint32_t i = a_next;
    a_next = (uint8_t)((a_next + 1u) % GM_N_ANALOGS);
    analog_read(i);
    for (uint32_t k = 0; k < GM_N_ANALOGS; k++)
#if GM_MSG_KINDS
        if (k != i && (gm_cfg.analog_mode[k] == AN_MODE_BEND || gm_cfg.analog_mode[k] == AN_MODE_PRESSURE))
#else
        if (k != i && gm_cfg.analog_mode[k] != AN_MODE_CC)
#endif
            analog_read(k);
}
#endif

/* ---- pads (velocity from strike force) --------------------------------- */

/* Every pad is read once per ms from the main loop, like the analog inputs
 * (with the hardware analog scan: its strongest reading of the ~5 sweeps
 * since the last pass, vpin_analog_peak):
 * they share the mux select lines and the one ADC, so reading them from the
 * scan interrupt would race with the main loop's reads. A hit: the reading
 * crosses pad.threshold, the peak over the next pad.window ms gives the
 * velocity (pad.threshold..pad.full -> 1..127, then pad.curve), and the pad
 * is let go below pad.release. A soft one (peak under twice pad.threshold)
 * that falls below pad.release again within PAD_MIN_MS is noise, not a hit:
 * the Code 61's pads see such spikes (velocity 1-3) while the pad board's
 * LEDs change. A hard short strike still counts.
 * pad.retrigger ms after a release must pass before the next hit. A pad that reads high at start-up stays silent until
 * it first reads below pad.release (wrong polarity, or held). */
#if GM_N_PADS > 0
#define NP GM_N_PADS
#define PAD_MS 1u
#define PAD_AT_MS 10u               /* poly aftertouch: at most once per 10 ms per pad */
#define PAD_MIN_MS 3u               /* a soft hit stays at or above pad.release this long */
enum { PAD_BLOCKED, PAD_IDLE, PAD_PEAK, PAD_ON };

static uint8_t pad_state[NP], pad_note[NP], pad_ch[NP], pad_at[NP];
static uint16_t pad_peak[NP];
static uint32_t pad_t[NP];          /* ms: strike start, last aftertouch, or release */
/* A hit lights its pad's LED (active look) for at least this long: a quick
 * strike can start and end between two 10 ms LED updates, which left the
 * pad on its idle look. */
#define PAD_LED_MIN_MS 80u
static uint32_t pad_lit[NP];        /* ms of the last hit (| 1), 0 = none */
static uint8_t pad_order[NP];       /* read order: same mux channel together */
/* Pad twins (core/README.md "Button layers"): a hit goes to the pad's live
 * twin, if any: an EVENT with the twin's index, no note. Its release goes
 * to whichever took the hit. */
#if GM_PAD_TWINS
static uint8_t pad_live[NP];        /* live twin + 1, 0 = the pad itself (layers_update) */
static uint8_t pad_owner[NP];       /* who took the last hit: twin + 1, 0 = the pad */
#define PAD_OWNER(i) pad_owner[i]
#else
#define PAD_OWNER(i) 0u
#endif

/* Mux pads by channel, so pads behind shared select lines switch them once. */
static void pads_init(void)
{
    for (uint32_t i = 0; i < NP; i++) {
        uint32_t k = i;
        uint32_t ch = gm_pads[i].pin.hub ? gm_pads[i].pin.num : 0;
        while (k > 0) {
            const gm_pin_t *p = &gm_pads[pad_order[k - 1]].pin;
            if ((p->hub ? p->num : 0u) <= ch)
                break;
            pad_order[k] = pad_order[k - 1];
            k--;
        }
        pad_order[k] = (uint8_t)i;
        pad_state[i] = PAD_BLOCKED;
    }
}

static uint32_t pad_velocity(uint32_t peak)
{
    if (gm_cfg.pad_curve == CURVE_FIXED)
        return (uint32_t)gm_cfg.vel_fixed;
    int32_t lo = gm_cfg.pad_threshold, hi = gm_cfg.pad_full;
    if (hi <= lo)
        hi = lo + 1;
    int32_t v = 1 + 126 * ((int32_t)peak - lo) / (hi - lo);
    v = v < 1 ? 1 : v > 127 ? 127 : v;
    if (gm_cfg.pad_curve == CURVE_SOFT)       /* as the keys' curves */
        v = 127 - (127 - v) * (127 - v) / 127;
    else if (gm_cfg.pad_curve == CURVE_HARD)
        v = v * v / 127;
    return (uint32_t)(v < 1 ? 1 : v);
}

/* A pad's own hit plays its note (no EVENT); a twin's sends an EVENT. */
static void pad_hit(uint32_t i, uint32_t now)
{
    pad_state[i] = PAD_ON;
    pad_t[i] = now;
    pad_lit[i] = now | 1u;
#if GM_PAD_TWINS
    pad_owner[i] = pad_live[i];
    if (pad_owner[i]) {
        event(EV_PAD, NP + pad_owner[i] - 1u, (uint32_t)pad_peak[i] << 2);
        return;
    }
#endif
    int32_t n = gm_cfg.pad_note[i] + 12 * gm_cfg.pad_octave;
    while (n > 127)
        n -= 12;                        /* off the top or bottom: the octave in range */
    while (n < 0)
        n += 12;
    uint32_t v = pad_velocity(pad_peak[i]);
    pad_at[i] = 0;
    pad_ch[i] = gm_cfg.pad_channel ? (uint8_t)((gm_cfg.pad_channel - 1) & 0x0F) : chan();
    pad_note[i] = (uint8_t)n;
    midi3(0x90 | pad_ch[i], (uint32_t)n, v);
}

/* The end of a hit: Note Off (and pressure 0), or the twin's EVENT 0. */
static void pad_end(uint32_t i)
{
#if GM_PAD_TWINS
    if (pad_owner[i]) {
        event(EV_PAD, NP + pad_owner[i] - 1u, 0);
        return;
    }
#endif
    if (pad_at[i])
        midi3(0xA0 | pad_ch[i], pad_note[i], 0);
    midi3(0x80 | pad_ch[i], pad_note[i], 0);
}

static void pad_off(uint32_t i, uint32_t now)
{
    pad_end(i);
    pad_state[i] = PAD_IDLE;
    pad_t[i] = now;
}

/* Poly pressure from the reading while held: release..full -> 0..127. */
static void pad_pressure(uint32_t i, int32_t r, uint32_t now)
{
    if (now - pad_t[i] < PAD_AT_MS)
        return;
    int32_t lo = gm_cfg.pad_release, hi = gm_cfg.pad_full;
    if (hi <= lo)
        hi = lo + 1;
    int32_t v = (r - lo) * 127 / (hi - lo);
    v = v < 0 ? 0 : v > 127 ? 127 : v;
    int32_t d = v - pad_at[i];
    if (d < 2 && d > -2 && !(v == 0 && pad_at[i]))
        return;
    pad_at[i] = (uint8_t)v;
    pad_t[i] = now;
    midi3(0xA0 | pad_ch[i], pad_note[i], (uint32_t)v);
}

#if GM_MONITOR
static uint16_t pad_raw[NP];        /* the last reading: RAW (sysex.md 4.8) */
#define PAD_RAW(i, r) (pad_raw[i] = (uint16_t)(r))
#else
#define PAD_RAW(i, r) ((void)0)
#endif

static void pads_scan(uint32_t now)
{
    int32_t on = gm_cfg.pad_threshold, off = gm_cfg.pad_release;
    if (off >= on)
        off = on - 1;
    vpin_analog_peaks();            /* hardware analog scan: peaks since the last pass */
    for (uint32_t k = 0; k < NP; k++) {
        uint32_t i = pad_order[k];
        int32_t r = vpin_analog_peak(gm_pads[i].pin);
        if (r < 0)
            continue;
        PAD_RAW(i, r);
        switch (pad_state[i]) {
        case PAD_IDLE:
            if (r < on || now - pad_t[i] < (uint32_t)gm_cfg.pad_retrigger)
                break;
            pad_state[i] = PAD_PEAK;
            pad_t[i] = now;
            pad_peak[i] = (uint16_t)r;
            break;
        case PAD_PEAK:
            if (r < off && now - pad_t[i] < PAD_MIN_MS && pad_peak[i] < 2 * on) {
                pad_state[i] = PAD_IDLE;    /* a soft spike, gone again at once */
                break;
            }
            if (r > pad_peak[i])
                pad_peak[i] = (uint16_t)r;
            if (r >= off && now - pad_t[i] < (uint32_t)gm_cfg.pad_window)
                break;
            pad_hit(i, now);            /* window over, or already let go */
            /* fall through */
        case PAD_ON:
            if (r < off)
                pad_off(i, now);
            else if (gm_cfg.pad_aftertouch && !PAD_OWNER(i))
                pad_pressure(i, r, now);
            break;
        default:                        /* PAD_BLOCKED */
            if (r < off) {
                pad_state[i] = PAD_IDLE;
                pad_t[i] = now;
            }
            break;
        }
    }
}

/* Note Off for every sounding pad (a twin's: EVENT 0); held pads stay
 * silent until let go. */
static void pads_off(void)
{
    for (uint32_t i = 0; i < NP; i++) {
        if (pad_state[i] == PAD_ON || pad_state[i] == PAD_PEAK) {
            if (pad_state[i] == PAD_ON)
                pad_end(i);
            pad_state[i] = PAD_BLOCKED;
        }
    }
}

/* At boot-log time: say which pads never read below pad.release. */
static void pads_report(void)
{
    for (uint32_t i = 0; i < NP; i++)
        if (pad_state[i] == PAD_BLOCKED)
            app_log(LOG_WARN, gm_pads[i].name, ": reads above pad.release at rest (inverted? held?)");
}
#endif

/* ---- encoders (main side) --------------------------------------------- */

#if GM_N_ENCODERS > 0
#if GM_ENC_ACCEL
/* Acceleration (core/README.md "Encoder acceleration"), per scanned encoder
 * (its twins share it): when it last turned (ms), the smoothed time between
 * its detents (1/16 ms), the fraction of a step carried to its next turn
 * (1/16 step) and the direction it turns. Main loop only. */
static uint32_t acc_t[GM_ARR(NE_HW)];
static uint16_t acc_avg[GM_ARR(NE_HW)];
static uint8_t acc_rem[GM_ARR(NE_HW)];
static int8_t acc_dir[GM_ARR(NE_HW)];

/* d detents of scanned encoder p, read at now: the steps they move. The
 * first detent after a pause of at least slow, and after a reversal, starts
 * from 1 step per detent; a reversal also drops the carried fraction, so
 * turning back is exact. Faster turns move up to max steps per detent, and
 * a detent always moves at least 1 step. Divides only on a turn. */
static int32_t enc_accel(uint32_t p, int32_t d, uint32_t now)
{
    const gm_enc_accel_t *a = &gm_enc_accel[p];
    if (a->max <= 1)
        return d;
    uint32_t n = (uint32_t)(d < 0 ? -d : d);
    int8_t dir = d < 0 ? -1 : 1;
    uint32_t slow = a->slow_ms * 16u, fast = a->fast_ms * 16u;
    uint32_t dt = now - acc_t[p];
    acc_t[p] = now;
    if (dir != acc_dir[p] || dt >= a->slow_ms) {
        acc_dir[p] = dir;
        acc_avg[p] = (uint16_t)slow;
        acc_rem[p] = 0;
    } else {
        uint32_t t = n == 1 ? dt * 16u : dt * 16u / n;     /* this turn's time per detent */
        acc_avg[p] = (uint16_t)((acc_avg[p] + t) / 2u);   /* t < slow here */
    }
    uint32_t avg = acc_avg[p], m = 16;  /* steps per detent, 1/16 step */
    if (avg < slow) {
        /* x: 0 at slow .. 256 at fast and below */
        uint32_t x = avg <= fast ? 256u : (slow - avg) * 256u / (slow - fast);
        if (a->curve == GM_ACCEL_QUADRATIC)
            x = x * x >> 8;
        else if (a->curve == GM_ACCEL_CUBIC)
            x = x * x * x >> 16;
        m += (a->max - 1u) * 16u * x >> 8;
    }
    uint32_t steps = n * m + acc_rem[p];
    acc_rem[p] = (uint8_t)(steps & 15u);
    steps >>= 4;
    return dir < 0 ? -(int32_t)steps : (int32_t)steps;
}
#endif

/* A scanned encoder's turns go to the live one of it and its twins: its
 * CC, mode, value and EVENT index. With acceleration, all of these get the
 * accelerated steps (DAW scripts nudge by the EVENT's delta). */
static void encoders_poll(uint32_t now)
{
    for (uint32_t p = 0; p < NE_HW; p++) {
        if (enc_i2c[p])
            encoder_step(p);
        uint16_t c = enc_count[p];
        int16_t raw = (int16_t)(uint16_t)(c - enc_seen[p]);
        if (!raw)
            continue;
        enc_seen[p] = c;
        uint32_t i = ENC_LIVE(p);
#if GM_ENC_ACCEL
        int32_t d = enc_accel(p, raw, now);
#else
        int16_t d = raw;            /* as before acceleration: the same code */
        (void)now;
#endif
        event(EV_ENCODER, i, (uint32_t)d & 0x3FFFu);
#if GM_MCU
        if (gm_encoder_mcu[i] != GM_MCU_NONE) {
            gm_mcu_vpot(gm_encoder_mcu[i], d);      /* it sends at most 15 */
            continue;
        }
#endif
        if (gm_cfg.enc_cc[i] < 0)
            continue;               /* a twin's cc: none: the EVENT only */
        uint32_t num = (uint32_t)gm_cfg.enc_cc[i];
        uint8_t st = (uint8_t)(0xB0u | ENC_CH(i));
        int32_t r = d;
#if GM_ENC_CTL
        const gm_ctl_t *t = &gm_encoder_ctl[i];
        if (t->max != GM_CTL_TOP && t->max < t->min)
            r = -r;                 /* relative, min > max: the other way */
#endif
        r = r > 63 ? 63 : r < -64 ? -64 : r;
        switch (gm_cfg.enc_mode[i]) {
        case ENC_REL_TWOS:
            midi3(st, num, (uint32_t)r & 0x7F);
            break;
        case ENC_REL_OFFSET:
            midi3(st, num, (uint32_t)(64 + r));
            break;
        case ENC_REL_SIGN:          /* 1..63 up, 65..127 down */
            midi3(st, num, r < 0 ? 0x40u | (uint32_t)(r < -63 ? 63 : -r) : (uint32_t)r);
            break;
        default: {
            /* absolute: 1 step per detent, 1/256 of the range in 14 bits */
            int32_t top = enc_top(i), span = enc_span(i, top);
            int32_t v = enc_value[i] + d * (span > 255 ? span >> 8 : 1);
            v = v < 0 ? 0 : v > span ? span : v;
            if (v != enc_value[i]) {
                enc_value[i] = (int16_t)v;
#if GM_MSG_KINDS
                int32_t mode = gm_cfg.enc_mode[i];
                send_value(mode >= ENC_CC14 ? (uint32_t)mode - 3u : K_CC, ENC_CH(i), num, enc_out(i, v, top));
#else
                midi3(st, num, enc_out(i, v, top));
#endif
            }
            break;
        }
        }
    }
}
#endif

/* ---- LEDs ------------------------------------------------------------- */

#ifndef MODE_LED
#define MODE_LED(li, level, now) (level)
#endif

/* Levels: 0 off, 1 on (pin), 2 both pins, 3 pin2, 4+ pin2 blinking
 * (bicolour). LV_BLINK | level: that level, blinking (host feedback).
 * RGB LEDs: LV_RGB | colour (bit 0 red, 1 green, 2 blue), from FB_LED_RGB;
 * any other level > 0 is white. Off (0) and a plain on (1; any on level of
 * a single-colour or RGB LED) show the LED's idle and active looks
 * (gm_led_t.looks); a blink's dark half is the idle look. */
/* Light the pins in mask m (bit 0 pin, 1 pin2, 2 pin3), the others dark. */
static void led_pins(const gm_led_t *l, uint32_t m)
{
    vpin_write(l->pin, m & 1u);
    if (!vpin_is_none(l->pin2))
        vpin_write(l->pin2, (m >> 1) & 1u);
    if (!vpin_is_none(l->pin3))
        vpin_write(l->pin3, (m >> 2) & 1u);
}

/* The LED's idle (active = 0) or active look, as a pin mask. */
static uint32_t led_look(const gm_led_t *l, int active)
{
    if (!(l->looks & 0x80u))
        return active ? (vpin_is_none(l->pin3) ? 1u : 7u) : 0u;
    return active ? (l->looks >> 3) & 7u : l->looks & 7u;
}

#if GM_HOST_FB
/* An RGB LED whose pins are one LED of a full-colour hub (VPIN_RGB: red,
 * green, blue in a row; the hub's rgb op checks that pin is an LED's first):
 * it can show any colour (vpin_rgb). */
static int led_full_colour(const gm_led_t *l)
{
    return (vpin_pin_caps(l->pin) & VPIN_RGB) &&
           l->pin2.hub == l->pin.hub && l->pin3.hub == l->pin.hub &&
           l->pin2.num == l->pin.num + 1u && l->pin3.num == l->pin.num + 2u;
}
#endif

static void led_show(const gm_led_t *l, uint32_t level, int blink_on)
{
#if GM_HOST_LEDS
    if (level & LV_BLINK)
        level = blink_on ? level & ~LV_BLINK : 0;
#endif
    uint32_t m;
    int look = -1;                  /* 0 idle, 1 active: m is that look */
    if (level == 0)
        m = led_look(l, look = 0);
    else if (level & LV_RGB) {
#if GM_HOST_FB
        if ((level & LV_COLOUR) && led_full_colour(l) &&
            vpin_rgb(l->pin, led_colour[(uint32_t)(l - gm_device.leds)]))
            return;
#endif
#if GM_MODE_RGB
        /* a step's full colour: the builder gives one only to an LED that
         * can show it (led_full_colour) */
        if ((level >> LV_STEP_SHIFT) && vpin_rgb(l->pin, gm_mode_rgb[(level >> LV_STEP_SHIFT) - 1u]))
            return;
#endif
        m = level & 7u;             /* these pins, whatever the kind */
    } else if (!vpin_is_none(l->pin3))
        m = led_look(l, look = 1);
    else if (vpin_is_none(l->pin2) || level == 1)
        m = led_look(l, look = 1);
    else                            /* bicolour 2 both, 3 pin2, 4+ pin2 blinking */
        m = level == 2 ? 3u : (level == 3 || blink_on) ? 2u : led_look(l, look = 0);
    if (look >= 0 && l->rgb[look] && vpin_rgb(l->pin, l->rgb[look]))
        return;                     /* a full-colour look (a full-colour hub's LEDs) */
    led_pins(l, m);
}

#if GM_HOST_LEDS
#define NL GM_ARR(GM_N_LEDS)
#define HOST_NONE 0xFFu
/* Level the host set (LV_* bits and 0..7), HOST_NONE = none, with two bits
 * no level uses: */
#define HOST_HOLD 0x08u             /* set by a DAW script: no timeout       */
#define HOST_BLACK 0x10u            /* ... its FB_LED_RGB black (leds_fade)  */
#define HOST_LEVEL(h) ((h) & ~(HOST_HOLD | HOST_BLACK))
static uint8_t led_host_lv[NL];
static uint16_t led_host_t[NL];     /* when: hal_millis() / 10 (the timeouts' unit) */

void gm_led_host(uint32_t led, uint32_t value)
{
    if (led >= gm_device.n_leds)
        return;
    led_host_lv[led] = (uint8_t)(value == 0 ? 0 : value == 1 ? (LV_BLINK | 1u) : 1u);
    led_host_t[led] = (uint16_t)(hal_millis() / 10u);
}

#if GM_HOST_FB
static int bicolour(uint32_t led)
{
    return !vpin_is_none(gm_device.leds[led].pin2) && vpin_is_none(gm_device.leds[led].pin3);
}

static void led_script_set(uint32_t led, uint32_t level)
{
    led_host_lv[led] = (uint8_t)(level | HOST_HOLD);
    led_host_t[led] = (uint16_t)(hal_millis() / 10u);
}

/* FB_LED levels (sysex.md 4.6.2): 0 off, 1 blink, 2..127 on; on a bicolour
 * LED 2..63 is colour 1 (level 1, its pin) and 64..127 colour 2 (level 3). */
void gm_led_script(uint32_t led, uint32_t level)
{
    if (led >= gm_device.n_leds)
        return;
    uint32_t lv = level == 0 ? 0 : level == 1 ? (LV_BLINK | 1u) : level >= 64 && bicolour(led) ? 3u : 1u;
    led_script_set(led, lv);
}

void gm_led_script_rgb(uint32_t led, uint32_t r, uint32_t g, uint32_t b, int blink)
{
    if (led >= gm_device.n_leds)
        return;
    uint32_t lv = 0, m = r > g ? r : g;
    if (b > m)
        m = b;
    if (!vpin_is_none(gm_device.leds[led].pin3)) {
        /* on/off per channel: lit if at least half the brightest one; an
         * LED that can show any colour gets the colour itself (0..127 per
         * channel to 0..255) */
        if (m) {
            lv = LV_RGB | (2u * r >= m) | (2u * g >= m) << 1 | (2u * b >= m) << 2;
            /* fade: chips dim with the colour too (leds_fade) */
            int full = led_full_colour(&gm_device.leds[led]);
            if (full || GM_TM_FADE) {
                uint32_t c = (r * 2u + (r >> 6)) << 16 | (g * 2u + (g >> 6)) << 8 | (b * 2u + (b >> 6));
                /* A new colour at the same level: draw it. Not on an on/off
                 * LED (fade: its pins don't change, and leds_fade must keep
                 * seeing it, or the chip jumps to full until it is drawn). */
                if (c != led_colour[led] && full)
                    led_shown[led] = 0xFFFFu;
                led_colour[led] = c;
                lv |= LV_COLOUR;
            }
        }
    } else if (m) {
        lv = bicolour(led) && g > r ? 3u : 1u;
    }
    /* black: a colour at brightness 0 */
    led_script_set(led, lv && blink ? (LV_BLINK | lv) : lv ? lv : HOST_BLACK);
}
#endif

void gm_host_release(uint32_t what)
{
    if (what == 0 || what == 3)
        gm_display_host_release();
    if (what == 0 || what == 1)
        for (uint32_t i = 0; i < gm_device.n_leds; i++)
            led_host_lv[i] = HOST_NONE;
#if GM_N_ANALOGS > 0 && GM_ANALOG_PICKUP
    if (what == 0 || what == 2)
        for (uint32_t i = 0; i < GM_N_ANALOGS; i++)
            a_pick[i] = -1;
#endif
}

/* Who drives LED i (core/README.md "LED ownership"). local = the device's
 * own state for it (its button's), LOCAL_HAS set; an LED no button uses has
 * no local state, so the host always drives it. now10 = hal_millis() / 10. */
#define LOCAL_HAS 0x80u
static uint32_t led_level(uint32_t i, uint32_t local, uint32_t now10)
{
    int has_local = (local & LOCAL_HAS) != 0;
    local &= ~LOCAL_HAS;
#if GM_LED_FB
    uint32_t owner = gm_led_fb[i].owner, timeout = gm_led_fb[i].timeout;
#else
    uint32_t owner = GM_LED_OWNER, timeout = GM_LED_TIMEOUT;
#endif
    uint32_t h = led_host_lv[i];
    if (owner == GM_OWNER_LOCAL)
        return local;
    if (owner == GM_OWNER_HOST || !has_local)
        return h == HOST_NONE ? 0 : HOST_LEVEL(h);
    if (h & HOST_HOLD)
        timeout = GM_TIMEOUT_NEVER;         /* a DAW script's state stays until released */
    if (h != HOST_NONE && timeout != GM_TIMEOUT_NEVER && (uint16_t)(now10 - led_host_t[i]) >= timeout)
        led_host_lv[i] = (uint8_t)(h = HOST_NONE);   /* host went quiet: local again */
    return h == HOST_NONE ? local : HOST_LEVEL(h);
}
#endif

/* What each LED last showed: (level << 1) | blink phase, 0xFFFF = not known
 * (start, after the lamp test). leds_update() runs every 10 ms over every
 * LED; an LED whose level and phase didn't change isn't written again. */
static uint16_t led_shown[GM_ARR(GM_N_LEDS)];   /* (declared above) */

/* Whether level shows differently in the two blink phases (led_show). */
static int led_blinks(const gm_led_t *l, uint32_t level)
{
#if GM_HOST_LEDS
    if (level & LV_BLINK)
        return 1;
#endif
    return !vpin_is_none(l->pin2) && vpin_is_none(l->pin3) && level >= 4u && !(level & LV_RGB);
}

#if GM_LAYERS
static uint8_t led_dead[GM_ARR(GM_N_LEDS)];   /* a layer twin (or its base) not shown now */
#endif

static void led_set(uint32_t i, uint32_t level, int blink_on)
{
#if GM_LAYERS
    if (led_dead[i])
        return;                     /* its twin has the pins */
#endif
#if GM_STATUS_LED >= 0
    if (i == (uint32_t)GM_STATUS_LED && gm_status_level() >= 0) {
        level = (uint32_t)gm_status_level();    /* a blink code (gm_safety.h) */
        blink_on = 0;
    }
#endif
    /* The blink phase is only part of the key for a blinking level: a
     * steady LED isn't written again every 500 ms. */
    int ph = led_blinks(&gm_device.leds[i], level) && blink_on;
    uint16_t key = (uint16_t)(((level & 0x7FFFu) << 1) | (ph ? 1u : 0u));
    if (led_shown[i] == key)
        return;
    led_shown[i] = key;
    led_show(&gm_device.leds[i], level, blink_on);
}

#if GM_LAYER_PARAMS
/* Which of each base and its twins is live (core/README.md "Button layers"):
 * the twin of the strongest layer that is on (layer_rank), else the base.
 * An LED that changes hands is drawn again. Then EVENT 8 for each mode
 * that went off (0), then each that came on (1). */
static void layers_update(void)
{
#if GM_LAYERS
#define RANK(layer) ((layer) ? layer_rank((layer) - 1u) : 0u)
#if GM_PAD_TWINS
    uint8_t pr[GM_N_PADS];
    for (uint32_t i = 0; i < GM_N_PADS; i++)
        pad_live[i] = pr[i] = 0;
    for (uint32_t t = 0; t < GM_PAD_TWINS; t++) {
        uint32_t b = gm_pad_twins[t].base, r = RANK(gm_pad_twins[t].layer);
        if (r > pr[b]) {
            pr[b] = (uint8_t)r;
            pad_live[b] = (uint8_t)(t + 1u);
        }
    }
#endif
#if GM_ENC_TWINS
    uint8_t er[NE_HW];
    for (uint32_t i = 0; i < NE_HW; i++) {
        enc_live[i] = (uint8_t)i;
        er[i] = 0;
    }
    for (uint32_t i = NE_HW; i < GM_N_ENCODERS; i++) {
        uint32_t b = gm_device.encoders[i].base, r = RANK(gm_device.encoders[i].layer);
        if (r > er[b]) {
            er[b] = (uint8_t)r;
            enc_live[b] = (uint8_t)i;
        }
    }
#endif
    /* buttons: the twins are the last GM_BTN_TWINS */
    uint8_t br[NB], bl[NB];
    for (uint32_t i = 0; i < GM_N_BUTTONS; i++) {
        br[i] = 0;
        bl[i] = (uint8_t)i;
    }
    for (uint32_t i = GM_N_OWN_BUTTONS; i < GM_N_BUTTONS; i++) {
        uint32_t b = gm_device.buttons[i].base, r = RANK(gm_device.buttons[i].layer);
        if (r > br[b]) {
            br[b] = (uint8_t)r;
            bl[b] = (uint8_t)i;
        }
    }
    uint64_t live = 0;
    for (uint32_t i = 0; i < GM_N_OWN_BUTTONS && i < 64; i++)
        live |= 1ull << bl[i];
    btn_live = live;
    uint8_t lr[GM_ARR(GM_N_LEDS)], ll[GM_ARR(GM_N_LEDS)];
    for (uint32_t i = 0; i < GM_N_LEDS; i++) {
        lr[i] = 0;
        ll[i] = (uint8_t)i;
    }
    for (uint32_t i = 0; i < GM_N_LEDS; i++) {
        const gm_led_t *l = &gm_device.leds[i];
        uint32_t r = RANK(l->layer);
        if (l->layer && r > lr[l->base]) {
            lr[l->base] = (uint8_t)r;
            ll[l->base] = (uint8_t)i;
        }
    }
    for (uint32_t i = 0; i < GM_N_LEDS; i++) {
        const gm_led_t *l = &gm_device.leds[i];
        uint8_t dead = ll[l->layer ? l->base : i] != i;
        if (led_dead[i] != dead) {
            led_dead[i] = dead;
            led_shown[i] = 0xFFFFu;     /* drawn again once it is live */
        }
    }
#undef RANK
#endif
    uint64_t on = 0;
    for (uint32_t m = 0; m < GM_N_MODES; m++)
        if (layer_rank(m))
            on |= 1ull << m;
    uint64_t d = on ^ modes_was;
    modes_was = on;
    for (uint32_t v = 0; v < 2u; v++)
        for (uint32_t m = 0; m < GM_N_MODES; m++)
            if (((d >> m) & 1u) && ((on >> m) & 1u) == v)
                event(EV_LAYER, m, v);
}
#endif

void gm_layers_report(void)
{
#if GM_LAYER_PARAMS
    report_due = 1;
#endif
}

#if GM_HOST_LEDS
/* leds_update() works out the buttons' and pads' own levels every 10 ms;
 * leds_apply() then shows LED_STEP LEDs per main-loop pass, so no one pass
 * carries all of them. */
#define LED_STEP 13u
static uint8_t local[NL];           /* level, | LOCAL_HAS: a button or pad has it */
static uint8_t led_next = 0xFFu;    /* next LED to show, 0xFF = none */
static uint32_t led_now, led_now10;
#endif

/* Diagnostic LED walk (GM_LED_WALK_MS, e.g. `make LED_WALK=500`): one LED
 * on at a time, each for GM_LED_WALK_MS, in leds: order, logging "walk LED
 * <n>". Holding any button pauses it. Normal LED behaviour is off. */
#ifndef GM_LED_WALK_MS
#define GM_LED_WALK_MS 0
#endif
#if GM_LED_WALK_MS
static void leds_walk(uint32_t now)
{
    static uint32_t step = 0xFFFFFFFFu, t_step;
    int paused = 0;
#if GM_N_BUTTONS > 0
    paused = btn_stable != 0;
#endif
    if (gm_device.n_leds == 0)
        return;
    if (step == 0xFFFFFFFFu || (!paused && now - t_step >= GM_LED_WALK_MS)) {
        step = step + 1u >= gm_device.n_leds ? 0 : step + 1u;
        t_step = now;
        char msg[16] = "LED ";
        char d[10];
        uint32_t n = step, k = 0, j = 4;
        do { d[k++] = (char)('0' + n % 10u); n /= 10u; } while (n);
        while (k)
            msg[j++] = d[--k];
        msg[j] = 0;
        app_log(LOG_INFO, "walk ", msg);
    }
    if (paused)
        t_step = now;
    for (uint32_t i = 0; i < gm_device.n_leds; i++)
        led_pins(&gm_device.leds[i], i == step ? 7u : 0u);   /* every pin, whatever its looks */
}
#endif

#if GM_TM_FADE && GM_HOST_FB
/* tm1628 fade: (vpin_fade): how bright each LED shows, from led_shown: a
 * DAW's colour at its brightest channel (these win), any other lit level or
 * look full. Run when leds_apply() has shown a whole round, and the TM1628
 * bytes wait for that (gm_poll), so a chip's new brightness goes out with
 * the round's bytes (tm_flush sends the brightness first), not a round
 * later: a dim host colour never shows at the old, full brightness. */
#define LEDS_FADE 1
/* LEDs on a fade chip (bit = LED), gm_init: the others aren't looked at. */
static uint32_t fade_leds[(GM_ARR(GM_N_LEDS) + 31u) / 32u];

static void leds_fade(void)
{
    for (uint32_t i = 0; i < gm_device.n_leds; i++) {
        const gm_led_t *l = &gm_device.leds[i];
        uint32_t key = led_shown[i], level = key >> 1;
        if (!((fade_leds[i >> 5] >> (i & 31u)) & 1u) || key == 0xFFFFu)
            continue;
#if GM_HOST_LEDS
        /* A blink's dark half shows the idle look, but a host colour's keeps
         * its brightness: else the whole chip pumps with the blink. */
        if ((level & LV_BLINK) && !(key & 1u) && !(level & LV_COLOUR))
            level = 0;
        level &= ~LV_BLINK;
#endif
        uint32_t v = 255u, colour = (level & LV_COLOUR) != 0;
        if (colour) {
            uint32_t c = led_colour[i], r = c >> 16, g = (c >> 8) & 0xFFu, b = c & 0xFFu;
            v = r > g ? r : g;
            if (b > v)
                v = b;
        } else if (level == 0 && led_host_lv[i] == (HOST_HOLD | HOST_BLACK)) {
            colour = 1;             /* a host's black: brightness 0, not its idle look */
            v = 0;
        } else if (level == 0 && !led_look(l, 0) && !l->rgb[0])
            continue;               /* dark */
        vpin_fade(l->pin, v, (int)colour);
    }
    vpin_fade_apply();
}
#else
#define leds_fade() ((void)0)
#define LEDS_FADE 0
#endif

static void leds_update(uint32_t now)
{
#if GM_LED_WALK_MS
    leds_walk(now);
    return;
#endif
    int blink_on = (now / 500u) & 1u;
#if GM_STATUS_LED >= 0
    /* A blink code on the status LED (gm_safety.h): drawn every update;
     * once it ends, the LED's own level is drawn again. */
    static uint8_t status_was;
    if (GM_STATUS_LED < (int)gm_device.n_leds) {
        if (gm_status_active())
            led_set(GM_STATUS_LED, 0, blink_on);
        else if (status_was)
            led_shown[GM_STATUS_LED] = 0xFFFFu;
        status_was = (uint8_t)gm_status_active();
    }
#endif
#if GM_HOST_LEDS
    for (uint32_t i = 0; i < NL; i++)
        local[i] = 0;
#endif
    for (uint32_t i = 0; i < gm_device.n_buttons; i++) {
        int8_t li = gm_device.buttons[i].led;
        if (li < 0 || (uint32_t)li >= gm_device.n_leds)
            continue;
        uint32_t level = 0;
#if GM_N_BUTTONS > 0
        int held = (btn_held >> i) & 1u;
        switch (gm_cfg.btn_action[i]) {
        case ACT_OCTAVE_DOWN:
            level = gm_cfg.octave < 0 ? (uint32_t)-gm_cfg.octave : 0;
            break;
        case ACT_OCTAVE_UP:
            level = gm_cfg.octave > 0 ? (uint32_t)gm_cfg.octave : 0;
            break;
        case ACT_TRANSPOSE_DOWN:
            level = gm_cfg.transpose < 0;
            break;
        case ACT_TRANSPOSE_UP:
            level = gm_cfg.transpose > 0;
            break;
        case ACT_CC_TOGGLE:
            level = (btn_toggled >> i) & 1u;
            break;
        case ACT_CC_RAMP:
            level = ramp_q8[i] > 0;
            break;
        case ACT_NONE:
            break;
        default:
            level = (uint32_t)held;
            break;
        }
#endif
#if GM_HOST_LEDS
        local[li] = (uint8_t)(level | LOCAL_HAS);
#else
        led_set((uint32_t)li, MODE_LED((uint32_t)li, level, now), blink_on);
#endif
    }
#if GM_N_PADS > 0
    /* A pad's LED is lit while its own hit is held; a twin's LED while the
     * twin's is (the base pad's state stays on the base LED). */
    for (uint32_t k = 0; k < NP + GM_PAD_TWINS; k++) {
        uint32_t i = k, owner = 0;
        int8_t li = gm_pads[k < NP ? k : 0].led;
#if GM_PAD_TWINS
        if (k >= NP) {
            i = gm_pad_twins[k - NP].base;
            li = gm_pad_twins[k - NP].led;
            owner = k - NP + 1u;
        }
#endif
        if (li < 0 || (uint32_t)li >= gm_device.n_leds)
            continue;
        uint32_t level = PAD_OWNER(i) == owner && (pad_state[i] == PAD_ON ||
                         (pad_lit[i] && now - pad_lit[i] < PAD_LED_MIN_MS));
#if GM_HOST_LEDS
        local[li] = (uint8_t)(level | LOCAL_HAS);
#else
        led_set((uint32_t)li, MODE_LED((uint32_t)li, level, now), blink_on);
#endif
    }
#endif
#if GM_N_MODES > 0 && !GM_HOST_LEDS
    for (uint32_t i = 0; i < gm_device.n_leds; i++) {
        uint32_t lv = mode_led(i, 0xFFu, now);    /* also LEDs no button drives */
        if (lv != 0xFFu)
            led_set(i, lv, blink_on);
    }
#endif
#if !GM_HOST_LEDS
    for (uint32_t i = 0; i < gm_device.n_leds; i++)
        if (led_shown[i] == 0xFFFFu)
            led_set(i, 0, blink_on);    /* driven by nothing: its idle look, once */
#endif
#if GM_HOST_LEDS
    (void)blink_on;                 /* leds_apply() works it out */
    led_now = now;
    led_now10 = now / 10u;
    led_next = 0;
#endif
}

/* Show the next LED_STEP LEDs of the round leds_update() started; 1 if it
 * did any. */
static int leds_apply(void)
{
#if GM_HOST_LEDS
    if (led_next >= gm_device.n_leds)
        return 0;
    uint32_t now = led_now, end = led_next + LED_STEP;
    int blink_on = (now / 500u) & 1u;
    for (uint32_t i = led_next; i < end && i < gm_device.n_leds; i++)
        led_set(i, MODE_LED(i, led_level(i, local[i], led_now10), now), blink_on);
    led_next = end >= gm_device.n_leds ? 0xFFu : (uint8_t)end;
    if (led_next == 0xFFu)
        leds_fade();                /* the round is shown: its brightness */
    return 1;
#else
    return 0;
#endif
}

/* ---- encoder rings (MCU V-Pots, DAW scripts) ----------------------------- */

#if GM_RINGS && GM_N_ENCODERS > 0
void gm_ring_draw(uint32_t e, uint32_t mode, int32_t frac, int center)
{
    const gm_ring_t *r = &gm_encoder_ring[e];
    int32_t n = r->n;
    if (r->center != GM_RING_NO_CENTER)
        gm_led_host(r->center, center ? 127 : 0);
    if (!n)
        return;
    if (frac > 16383)
        frac = 16383;
    int32_t p = frac < 0 ? -1 : (frac * (n - 1) + 8191) / 16383;   /* LED index of the position */
    int32_t c = (n - 1) / 2;
    for (int32_t k = 0; k < n; k++) {
        int on = 0;
        if (frac >= 0) {
            switch (mode & 3u) {
            case 0:
                on = k == p;
                break;
            case 1:
                on = (k >= c && k <= p) || (k <= c && k >= p);
                break;
            case 2:
                on = k <= p;
                break;
            default: {
                int32_t w = frac * (n / 2) / 16383;                /* width either side */
                on = k >= c - w && k <= c + w;
                break;
            }
            }
        }
        gm_led_host(r->leds[k], on ? 127 : 0);
    }
}
#else
void gm_ring_draw(uint32_t e, uint32_t mode, int32_t frac, int center)
{
    (void)e;
    (void)mode;
    (void)frac;
    (void)center;
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
            uint32_t want = f->channel ? f->channel - 1u : chan();
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
    if (ch != chan() || !fb_used(type, num))
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
    for (uint32_t i = 0; i < GM_N_BUTTONS && i < 64; i++) {
        int held = (btn_held >> i) & 1u;
        int32_t act = gm_cfg.btn_action[i];
        if (btn_note[i] != NOTE_NONE) {
            midi3(0x80 | btn_chan[i], btn_note[i], 0);
            btn_note[i] = NOTE_NONE;
        }
        if (held && act == ACT_CC_MOMENTARY)
            BTN_CC(i, (uint32_t)gm_cfg.btn_number[i], 0);
#if GM_MCU
        if (held && act == ACT_MCU)
            gm_mcu_note((uint32_t)gm_cfg.btn_number[i], 0);
#endif
        if (act == ACT_CC_RAMP && ramp_sent[i] != 0)
            BTN_CC(i, (uint32_t)gm_cfg.btn_number[i], 0);
        ramp_q8[i] = 0;
        ramp_sent[i] = 0;
    }
    btn_ignore = btn_held;
    btn_toggled = 0;
    if (bend_sent != 0x7FFFFFFF && bend_sent != 0)
        midi3(0xE0 | chan(), 0, 0x40);      /* centre */
    bend = bend_target = 0;
    bend_sent = 0;
#endif
#if GM_N_ANALOGS > 0 && GM_ANALOG_PICKUP
    for (uint32_t i = 0; i < NA; i++)
        a_pick[i] = -1;
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
    for (uint32_t g = 0; g < GM_N_CYCLES; g++)
        cyc_cur[g] = GM_MODE_NONE;  /* set once the settings are loaded (below) */
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
    for (uint32_t i = 0; i < NE_HW; i++) {
        const gm_encoder_t *e = &gm_device.encoders[i];
        vpin_input(e->a);
        vpin_input(e->b);
        enc_i2c[i] = (uint8_t)(vpin_on_i2c(e->a) || vpin_on_i2c(e->b));
        enc_ra[i] = vpin_in_reg(e->a, &enc_ma[i]);
        enc_rb[i] = vpin_in_reg(e->b, &enc_mb[i]);
    }
#endif
#if GM_HAS_MATRIX
    if (gm_device.matrix) {
        const gm_matrix_t *m = gm_device.matrix;
        rows_fast = m->n_rows <= FAST_ROWS;
#if GM_MATRIX_DECODER
        rows_dec = m->n_rows > 0;
        rows_sel = m->n_rows > 0 && m->n_rows <= FAST_ROWS;
#endif
        for (uint32_t r = 0; r < m->n_rows; r++) {
            vpin_output(m->rows[r]);
#if GM_MATRIX_DECODER
            if (!vpin_is_decoder(m->rows[r]))
                rows_dec = 0;
            if (r >= FAST_ROWS || !vpin_select_word(m->rows[r], &row_sel_reg[r], &row_sel_word[r]))
                rows_sel = 0;
#endif
            if (on_sr_hub(m->rows[r]))
                m_hubs = 1;
            if (r < FAST_ROWS) {
                row_reg[r] = m->rows[r].hub ? 0 : hal_pin_out_reg(m->rows[r].num);
                if (!row_reg[r])
                    rows_fast = 0;
                if (!(m->rows[r].flags & GM_PIN_INVERT))
                    row_on |= 1u << r;
            }
        }
        cols_fast = m->n_cols <= FAST_COLS;
        n_runs = 0;
        for (uint32_t c = 0; c < m->n_cols; c++) {
            vpin_input(m->cols[c]);
            if (on_sr_hub(m->cols[c]))
                m_hubs = 1;
            if (c >= FAST_COLS)
                continue;
            uint32_t mask = 0;
            const volatile uint32_t *reg = m->cols[c].hub ? 0 : hal_pin_in_reg(m->cols[c].num, &mask);
            if (!reg || !mask || (mask & (mask - 1u))) {
                cols_fast = 0;          /* no register, or not a single bit */
                continue;
            }
            uint8_t bit = (uint8_t)__builtin_ctz(mask);
            col_run_t *k = n_runs ? &col_run[n_runs - 1u] : 0;
            uint32_t len = k ? (uint32_t)__builtin_popcount(k->mask) : 0;
            if (k && k->reg == reg && k->src + len == bit && k->dst + len == c && len < 31u) {
                k->mask = (k->mask << 1) | 1u;      /* extends the previous run */
            } else if (n_runs < GM_ARR(GM_MATRIX_COL_RUNS)) {
                col_run[n_runs++] = (col_run_t){ reg, 1u, bit, (uint8_t)c };
            } else {
                cols_fast = 0;          /* more runs than the builder counted */
            }
            if (m->cols[c].flags & GM_PIN_INVERT)
                col_inv |= 1u << c;
        }
#if SCAN_FAST
        scan_fast_init(m);
#endif
        for (uint32_t k = 0; k < NK; k++)
            knote[k] = NOTE_NONE;
    }
#endif
#if GM_N_MODES > 0
    mode_cur = hold_cur = GM_MODE_NONE;
    show_n = 0;
    btn_in_mode = 0;
#endif
#if GM_LAYER_PARAMS
    /* layer-only toggles: no keys or buttons (so no sub-modes either) */
    solo_mask = solo_on = 0;
    for (uint32_t m = 0; m < GM_N_MODES; m++) {
        const gm_mode_t *md = &gm_modes[m];
        if (md->toggle != GM_MODE_NONE && !md->hold && !md->cycle && !md->n_keys && !md->n_buttons)
            solo_mask |= 1ull << m;
    }
    for (uint32_t w = 0; w < sizeof solo_leds / sizeof solo_leds[0]; w++)
        solo_leds[w] = 0;
    for (uint32_t m = 0; m < GM_N_MODES; m++) {
        const gm_mode_t *md = &gm_modes[m];
        if (md->led >= 0 && (uint32_t)md->led < gm_device.n_leds && (md->cycle || ((solo_mask >> m) & 1u)))
            solo_leds[(uint32_t)md->led >> 5] |= 1u << ((uint32_t)md->led & 31u);
    }
#endif
#if GM_N_BUTTONS > 0
    for (uint32_t i = 0; i < NB; i++) {
        btn_note[i] = NOTE_NONE;
        ramp_sent[i] = 0;
    }
#endif
#if GM_N_ANALOGS > 0
    for (uint32_t i = 0; i < NA; i++) {
        a_sent[i] = -1;
        bend_scale(i);
#if GM_ANALOG_PICKUP
        a_pick[i] = -1;
#endif
    }
#endif
#if GM_N_PADS > 0
    pads_init();
#endif

#if GM_HOST_LEDS
    for (uint32_t i = 0; i < NL; i++)
        led_host_lv[i] = HOST_NONE;
#endif
#if GM_TM_FADE && GM_HOST_FB
    for (uint32_t i = 0; i < gm_device.n_leds; i++)
        if (vpin_fades(gm_device.leds[i].pin))
            fade_leds[i >> 5] |= 1u << (i & 31u);
        else
            fade_leds[i >> 5] &= ~(1u << (i & 31u));
#endif
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
    for (uint32_t g = 0; g < GM_N_CYCLES; g++)
        cyc_cur[g] = (uint8_t)cycle_after(g + 1u, GM_MODE_NONE);   /* the first enabled step */
#endif
    layers_update();
    app_log_limit = (uint8_t)gm_cfg.log_level;
    app_proto_init(fw_version, 1);

    /* Lamp test: every LED on briefly. */
    for (uint32_t i = 0; i < gm_device.n_leds; i++)
        led_pins(&gm_device.leds[i], 7u);
    vpin_flush();
    vpin_flush_i2c();
    vpin_send_all();
#if GM_N_SCREENS > 0
    (void)gm_screen_send(0xFFFFFFFFu);
#endif
    hal_delay_us(200000);
    for (uint32_t i = 0; i < gm_device.n_leds; i++) {
        led_pins(&gm_device.leds[i], 0);
        led_shown[i] = 0xFFFFu;         /* leds_update writes each once (its idle look, if off) */
    }
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
    if (report_due) {               /* debug.events set to 1: every mode that is on */
        report_due = 0;
        for (uint32_t m = 0; m < GM_N_MODES; m++)
            if ((modes_was >> m) & 1u)
                event(EV_LAYER, m, 1);
    }
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
#if LEDS_FADE
    int round_open = led_next != 0xFFu;
#else
    int round_open = 0;
#endif
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
