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

#define BTN_SAMPLE_MS   1u
/* A press counts as soon as it is read, so a quick tap is never lost; a
 * release counts once the button has read released for BTN_STABLE samples in
 * a row (10 ms), which rides out contact bounce (4 ms was too short on the
 * Code 61: taps sometimes counted twice). */
#define BTN_STABLE      10u
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

#ifndef GM_N_MODES
#define GM_N_MODES 0                /* a config generated before modes */
#endif
#ifndef GM_HW_SCAN
#define GM_HW_SCAN 0                /* a config generated before hardware_scan: */
#endif
#if GM_N_MODES > 0 && (!GM_HAS_MATRIX || GM_N_BUTTONS == 0)
#error "modes need a key matrix and a toggle button (the builder checks this)"
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

static uint32_t fw_ver;
static uint8_t has_sr_hubs;         /* any 74HC595/165 hub: refresh in the tick */
static uint8_t has_matrices;        /* any switch-matrix hub: scan in the tick */
static uint8_t has_i2c_hubs;
static uint8_t has_tm_hubs;         /* TM1628: written after each LED update */
static uint8_t has_rgb_hubs;        /* RGB_SERIAL: polled on every pass */

/* ---- MIDI out --------------------------------------------------------- */

static uint8_t chan(void)
{
    return (uint8_t)((gm_cfg.channel - 1) & 0x0F);
}

static void midi3(uint8_t status, uint32_t a, uint32_t b)
{
    uint8_t m[3] = { status, (uint8_t)(a & 0x7F), (uint8_t)(b & 0x7F) };
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

static void cc(uint32_t num, uint32_t value)
{
    midi3(0xB0 | chan(), num, value);
}

static void event(uint8_t type, uint32_t index, uint32_t value)
{
    if (gm_cfg.events)
        app_event(type, index, value);
}

/* ---- key matrix (scan side) ------------------------------------------ */

#if GM_HAS_MATRIX
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
static uint32_t kt0[NK];
static uint32_t mprev[32];
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
static col_run_t col_run[FAST_COLS];
static uint8_t n_runs;
static uint32_t col_inv;            /* columns with GM_PIN_INVERT */
static uint8_t cols_fast;
#define FAST_ROWS 32u
static volatile uint32_t *row_reg[FAST_ROWS];
static uint32_t row_on;             /* level that drives row r: bit r */
static uint8_t rows_fast;
static uint8_t rows_dec;            /* every row is a decoder output */
/* Rows selected by one store each (vpin_select_word): decoder rows on a port
 * with a set/reset register. */
static volatile uint32_t *row_sel_reg[FAST_ROWS];
static uint32_t row_sel_word[FAST_ROWS];
static uint8_t rows_sel;
static uint8_t knote[NK], kchan[NK];
#endif

#if GM_N_MODES > 0
static uint8_t mode_cur = GM_MODE_NONE;     /* the active toggled mode; GM_MODE_NONE = none */
static uint8_t hold_cur = GM_MODE_NONE;     /* the hold mode whose button is held */
static uint32_t keys_mode(void);
static void mode_key(uint32_t mi, uint32_t k);
#endif
#if GM_LAYERS
static void layers_update(void);
#else
#define layers_update() ((void)0)
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
static void kq_push(uint32_t key, int on, uint32_t dt)
{
    uint8_t next = (uint8_t)((kq_head + 1u) & (KQ - 1u));
    if (next == kq_tail)
        return;                     /* full: main loop stalled for > 32 events */
    kq[kq_head].key = (uint8_t)key;
    kq[kq_head].on = (uint8_t)on;
    kq[kq_head].dt = (uint16_t)(dt > 0xFFFFu ? 0xFFFFu : dt);
    kq_head = next;
}

/* Two-contact key: whichever contact closes first starts the timer, both
 * closed = note on, both open = note off. */
static void key_update(uint32_t k, uint32_t c)
{
    switch (kstate[k]) {
    case K_IDLE:
        if (c == 3u) {
            kstate[k] = K_ON;       /* both at once: as fast as we can see */
            kq_push(k, 1, 0);
        } else if (c) {
            kstate[k] = K_TIMING;
            kt0[k] = ticks;
        }
        break;
    case K_TIMING:
        if (c == 3u) {
            kstate[k] = K_ON;
            kq_push(k, 1, ticks - kt0[k]);
        } else if (!c) {
            kstate[k] = K_IDLE;
        }
        break;
    default:
        if (!c) {
            kstate[k] = K_IDLE;
            kq_push(k, 0, 0);
        }
        break;
    }
}

/* A pin on a 74HC595/165 hub: the matrix scan must flush/refresh those
 * around each row. (A decoder row is written at once; nothing to flush.) */
static int on_sr_hub(gm_pin_t p)
{
    if (p.hub == 0 || p.hub > gm_device.n_hubs)
        return 0;
    uint8_t t = gm_device.hubs[p.hub - 1].type;
    return t == GM_HUB_HC595 || t == GM_HUB_HC165;
}

/* Key number at matrix position pos (gm_matrix_t.key_map), or dflt without a
 * map. GM_KEY_NONE (and anything >= n_keys) is no key. */
static uint32_t key_at(const gm_matrix_t *m, uint32_t pos, uint32_t dflt)
{
    return m->key_map ? m->key_map[pos] : dflt;
}

#if GM_HW_SCAN
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
        mprev[r - 1u] = *even;
        mprev[r] = s;
        uint32_t pair = r / 2u;
        for (uint32_t c = 0; c < m->n_cols; c++) {
            if (!((diff >> c) & 1u))
                continue;
            uint32_t k = key_at(m, pair * m->n_cols + c, pair + (rows / 2u) * c);
            if (k < m->n_keys)
                key_update(k, ((*even >> c) & 1u) | (((s >> c) & 1u) << 1));
        }
        return;
    }
    uint32_t diff = s ^ mprev[r];
    if (!diff)
        return;
    mprev[r] = s;
    if (m->kind == GM_MATRIX_KIND_VELOCITY) {
        for (uint32_t p = 0; 2u * p + 1u < m->n_cols; p++) {
            if (!((diff >> (2u * p)) & 3u))
                continue;
            uint32_t k = key_at(m, r * (m->n_cols / 2u) + p, r + rows * p);
            if (k < m->n_keys)
                key_update(k, (s >> (2u * p)) & 3u);
        }
    } else {
        for (uint32_t c = 0; c < m->n_cols; c++) {
            if (!((diff >> c) & 1u))
                continue;
            uint32_t k = key_at(m, r * m->n_cols + c, r + rows * c);
            if (k < m->n_keys)
                kq_push(k, (s >> c) & 1u, 0);
        }
    }
}

static void matrix_scan(void)
{
    const gm_matrix_t *m = gm_device.matrix;
    uint32_t rows = m->n_rows > 32 ? 32 : m->n_rows;
    int by_rows = m->kind == GM_MATRIX_KIND_VELOCITY && m->contacts == GM_MATRIX_CONTACTS_ROWS;
    uint32_t even = 0;              /* by_rows: this pass's reading of row r - 1 */
    for (uint32_t r = 0; r < rows; r++) {
        if (rows_sel)
            *row_sel_reg[r] = row_sel_word[r];
        else if (rows_fast)
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
static uint8_t enc_ab[NE];
static int8_t enc_acc[NE];
static volatile int16_t enc_delta[NE];
static uint8_t enc_i2c[NE];
static int16_t enc_value[NE];
/* One-load reads of A and B (vpin_in_reg), electrical levels; 0 = vpin_read. */
static const volatile uint32_t *enc_ra[NE], *enc_rb[NE];
static uint32_t enc_ma[NE], enc_mb[NE];
#endif
#if GM_ENC_TWINS
static uint8_t enc_live[NE];        /* per scanned encoder: the index its turns go to (layers_update) */
#define ENC_LIVE(i) enc_live[i]
#else
#define ENC_LIVE(i) (i)
#endif

void gm_encoder_host(uint32_t i, uint32_t value)
{
#if GM_N_ENCODERS > 0
    if (i < GM_N_ENCODERS && gm_cfg.enc_mode[i] == ENC_ABSOLUTE)
        enc_value[i] = (int16_t)(value > 127 ? 127 : value);
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
        enc_delta[i]++;
    } else if (enc_acc[i] <= -spd) {
        enc_acc[i] = 0;
        enc_delta[i]--;
    }
}
#endif

/* ---- hardware scan (gm_controller.h gm_scan_plan_t) -------------------- */

static uint8_t hw_on;               /* the platform scans; frames come to gm_scan_frames */
#if GM_HW_SCAN
static uint32_t hw_frame_q4;        /* frame period, 1/16 us */
static gm_scan_plan_t plan;
static uint32_t hw_prev[GM_SCAN_MAX_ROWS / 2u];   /* the last frame that differed */
static uint32_t hw_mask;            /* column bits of both halfwords of a frame word */
#if GM_N_ENCODERS > 0
static uint8_t enc_fed[NE];         /* both pins on switch matrices: stepped per frame */
#endif
typedef uint32_t __attribute__((may_alias)) u32_alias;

static int on_swmatrix(gm_pin_t p)
{
    return p.hub != 0 && p.hub <= gm_device.n_hubs && gm_device.hubs[p.hub - 1].type == GM_HUB_SWMATRIX;
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
        if (!rows_sel || !cols_fast || m_hubs || m->n_rows > GM_SCAN_MAX_ROWS) {
            app_log(LOG_WARN, "hardware scan: key matrix rows need one-store selects and ",
                    "columns on one input register; scanning in software");
            return 0;
        }
        for (uint32_t r = 0; r < m->n_rows; r++) {
            if (plan.sel_reg && row_sel_reg[r] != plan.sel_reg)
                goto mixed;
            plan.sel_reg = row_sel_reg[r];
            plan.sel_word[r] = row_sel_word[r];
        }
        for (uint32_t i = 0; i < n_runs; i++) {
            if (plan.in_reg && col_run[i].reg != plan.in_reg)
                goto mixed;
            plan.in_reg = col_run[i].reg;
            plan.in_mask |= col_run[i].mask << col_run[i].src;
        }
        plan.n_rows = m->n_rows;
    }
#endif
    if (!vpin_scan_plan(&plan.sel_reg, plan.sel_word, &plan.n_rows, GM_SCAN_MAX_ROWS,
                        &plan.in_reg, &plan.in_mask))
        goto mixed;
    if (!plan.n_rows || !plan.sel_reg || !plan.in_reg || (plan.in_mask >> 16)) {
        app_log(LOG_WARN, "hardware scan: nothing to scan, or columns above bit 15; ",
                "scanning in software");
        return 0;
    }
    if (plan.n_rows & 1u) {
        if (plan.n_rows >= GM_SCAN_MAX_ROWS)
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
    for (uint32_t i = 0; i < GM_SCAN_MAX_ROWS / 2u; i++)
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
    if (gm_device.matrix && !hw_on)
        matrix_scan();
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

/* Stock-like curve: 127 at vel_fast, 1 at vel_slow, linear in log(time)
 * between (about -18 velocity per doubling with the defaults). */
static uint32_t velocity(uint32_t dt_ticks)
{
    if (gm_cfg.vel_curve == CURVE_FIXED || gm_device.matrix->kind != GM_MATRIX_KIND_VELOCITY)
        return (uint32_t)gm_cfg.vel_fixed;
    /* In microseconds: a hardware scan's ticks (frames) are a few tens of
     * us, finer than the 0.1 ms steps of vel_fast / vel_slow. The tick
     * period is in 1/16 us; a product that won't fit 32 bits saturates. */
    uint32_t q4 = gm_scan_period_us * 16u;
#if GM_HW_SCAN
    if (hw_on)
        q4 = hw_frame_q4;
#endif
    uint32_t t = q4 && dt_ticks > 0xFFFFFFFFu / q4 ? 0x0FFFFFFFu : (dt_ticks * q4) >> 4;
    int32_t fast = gm_cfg.vel_fast * 100, slow = gm_cfg.vel_slow * 100;
    if (slow <= fast)
        slow = fast + 1;
    int32_t v;
    if ((int32_t)t <= fast) {
        v = 127;
    } else if ((int32_t)t >= slow) {
        v = 1;
    } else {
        int32_t lf = log2_q8((uint32_t)fast), ls = log2_q8((uint32_t)slow);
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
static uint64_t btn_hist[BTN_STABLE - 1u];  /* the samples before the latest */
static uint64_t btn_ignore;         /* held through a preset switch: release is silent */
static int32_t ramp_q8[NB];         /* CC ramp position, 0..127 << 8 */
static int16_t ramp_sent[NB];
static uint8_t btn_note[NB], btn_chan[NB];
static int32_t bend, bend_target, bend_sent = 0x7FFFFFFF;
static int32_t program;

static uint64_t read_buttons(void)
{
    uint64_t s = 0;
    for (uint32_t i = 0; i < GM_N_BUTTONS && i < 64; i++)
        if (vpin_read(gm_device.buttons[i].pin))
            s |= 1ull << i;
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
}

static void button_changed(uint32_t i, int down)
{
    int32_t act = gm_cfg.btn_action[i];
    uint32_t num = (uint32_t)gm_cfg.btn_number[i];
    event(EV_BUTTON, i, (uint32_t)down);
    app_log(LOG_DEBUG, gm_device.buttons[i].name, down ? " down" : " up");
    switch (act) {
    case ACT_CC_MOMENTARY:
        cc(num, down ? 127 : 0);
        break;
    case ACT_CC_TOGGLE:
        if (down) {
            btn_toggled ^= 1ull << i;
            cc(num, ((btn_toggled >> i) & 1u) ? 127 : 0);
        }
        break;
    case ACT_NOTE:
        if (down) {
            btn_note[i] = (uint8_t)num;
            btn_chan[i] = chan();
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
    case ACT_PROGRAM_NEXT:
    case ACT_PROGRAM_PREV:
        if (down) {
            program = (program + (act == ACT_PROGRAM_NEXT ? 1 : 127)) & 127;
            midi2(0xC0 | chan(), (uint32_t)program);
        }
        break;
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

/* The toggle or hold button of a root mode. Toggle: enters the mode (held
 * keys get their Note Off first, if it has keys), or leaves it from the
 * mode or any of its sub-modes. Hold: the mode is active while the button
 * is down. Its own action never runs. Returns 0 if button i is neither.
 * A twin of a toggle: { button, with } button toggles the mode as well. */
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
        event(EV_BUTTON, i, (uint32_t)down);
        if (md->hold) {
            if (down) {
                if (md->n_keys && keys_mode() == GM_MODE_NONE)
                    keys_off();
                hold_cur = (uint8_t)m;
                if (binds_channel(md->buttons, md->n_buttons) || binds_channel(md->keys, md->n_keys))
                    show_channel();
            } else if (hold_cur == m) {
                hold_cur = GM_MODE_NONE;
            }
            layers_update();
            app_log(LOG_DEBUG, gm_device.buttons[i].name, down ? ": mode on" : ": mode off");
            return 1;
        }
        if (!down)
            return 1;
        if (mode_cur != GM_MODE_NONE && gm_modes[mode_cur].root == m) {
            mode_cur = GM_MODE_NONE;
        } else {
            if (md->n_keys && keys_mode() == GM_MODE_NONE)
                keys_off();
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
static uint32_t mode_led(uint32_t li, uint32_t level, uint32_t now)
{
    if (show_n) {
        const gm_mode_t *m = &gm_modes[show_mode];
        uint32_t s = (now - show_t0) / SHOW_STEP_MS, n2 = 2u * show_n;
        int is_count = m->blink_count == (int8_t)li, is_done = m->blink_done == (int8_t)li;
        if (s > n2 + 2u)
            show_n = 0;             /* done: a dark step, N blinks, a 2-step flash */
        else if (is_count || is_done)
            return s > n2 ? 1u : is_count && (s & 1u);
    }
    if (hold_cur != GM_MODE_NONE && gm_modes[hold_cur].led == (int8_t)li)
        return gm_modes[hold_cur].led_blink ? (now / MODE_BLINK_MS) & 1u : 1u;
    if (mode_cur != GM_MODE_NONE && gm_modes[mode_cur].led == (int8_t)li)
        return gm_modes[mode_cur].led_blink ? (now / MODE_BLINK_MS) & 1u : 1u;
    return level;
}
#define MODE_LED(li, level, now) mode_led(li, level, now)
#endif

static void buttons_sample(void)
{
    uint64_t s = read_buttons();
    uint64_t recent = s;            /* pressed in any of the last BTN_STABLE samples */
    for (uint32_t k = 0; k < BTN_STABLE - 1u; k++)
        recent |= btn_hist[k];
    for (uint32_t k = BTN_STABLE - 1u; k > 1u; k--)
        btn_hist[k - 1u] = btn_hist[k - 2u];
    btn_hist[0] = s;
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
                    hold_cur = GM_MODE_NONE;  /* a hold mode still ends with its button */
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
static void ramps_ms(uint32_t now)
{
    for (uint32_t i = 0; i < GM_N_BUTTONS; i++) {
        if (gm_cfg.btn_action[i] != ACT_CC_RAMP)
            continue;
        int32_t target = ((btn_held >> i) & 1u) ? (127 << 8) : 0;
        int32_t step = gm_cfg.ramp_time > 0 ? (127 << 8) / gm_cfg.ramp_time : (127 << 8);
        if (step < 1)
            step = 1;
        if (ramp_q8[i] < target)
            ramp_q8[i] = ramp_q8[i] + step > target ? target : ramp_q8[i] + step;
        else if (ramp_q8[i] > target)
            ramp_q8[i] = ramp_q8[i] - step < target ? target : ramp_q8[i] - step;
        int16_t v = (int16_t)(ramp_q8[i] >> 8);
        if (v != ramp_sent[i]) {
            ramp_sent[i] = v;
            cc((uint32_t)gm_cfg.btn_number[i], (uint32_t)v);
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

/* Pitch bend from analog input i at pos (0..4095): centre to each end is
 * one half of the 14-bit range. */
static void analog_bend(uint32_t i, int32_t pos)
{
    const gm_analog_t *a = &gm_device.analogs[i];
    int32_t range = a->raw_max > a->raw_min ? a->raw_max - a->raw_min : a->raw_min - a->raw_max;
    if (range == 0)
        range = 4095;
    int32_t dead = BEND_DEAD * 4095 / range;          /* in 0..4095 units */
    int32_t step = BEND_STEP * 16384 / range;         /* in bend units */
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

static void analog_read(uint32_t i)
{
    const gm_analog_t *a = &gm_device.analogs[i];
    int32_t raw = vpin_analog(a->pin);
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
    if (a_sent[i] < 0 && !jump) {
        a_filt[i] = v << 2;                 /* first reading: no message */
        a_sent[i] = (int16_t)(v >> 5);
        a_center[i] = (int16_t)v;
        a_bend[i] = 8192;
        return;
    }
    if (jump)
        a_filt[i] = v << 2;
    else
        a_filt[i] += v - (a_filt[i] >> 2); /* 4-sample moving average */
    int32_t pos = a_filt[i] >> 2;           /* 0..4095 */
    if (gm_cfg.analog_mode[i] == AN_MODE_BEND) {
        analog_bend(i, pos);
        return;
    }
    int32_t center = a_sent[i] * 32 + 16;
    if (pos - center > 24 || center - pos > 24 ||
        (pos >= 4088 && a_sent[i] != 127) || (pos <= 8 && a_sent[i] != 0)) {
        int16_t q = (int16_t)(pos >> 5);
        if (q != a_sent[i]) {
            a_sent[i] = q;
#if GM_ANALOG_PICKUP
            if (a_pick[i] >= 0) {
                int side = q < a_pick[i] ? -1 : q > a_pick[i] ? 1 : 0;
                if (side == a_side[i])
                    return;                 /* not picked up yet */
                a_pick[i] = -1;
            }
#endif
#if GM_MCU
            if (gm_analog_mcu[i] != GM_MCU_NONE)
                gm_mcu_fader(gm_analog_mcu[i], (uint32_t)pos << 2);
            else
#endif
            if (gm_cfg.analog_mode[i] == AN_MODE_PRESSURE)
                midi2(0xD0 | chan(), (uint32_t)q);   /* channel pressure (aftertouch) */
            else
                cc((uint32_t)gm_cfg.analog_cc[i], (uint32_t)q);
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
        if (k != i && gm_cfg.analog_mode[k] != AN_MODE_CC)
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
 * is let go below pad.release. pad.retrigger ms after a release must pass
 * before the next hit. A pad that reads high at start-up stays silent until
 * it first reads below pad.release (wrong polarity, or held). */
#if GM_N_PADS > 0
#define NP GM_N_PADS
#define PAD_MS 1u
#define PAD_AT_MS 10u               /* poly aftertouch: at most once per 10 ms per pad */
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
    int32_t n = gm_cfg.pad_note[i];
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
        switch (pad_state[i]) {
        case PAD_IDLE:
            if (r < on || now - pad_t[i] < (uint32_t)gm_cfg.pad_retrigger)
                break;
            pad_state[i] = PAD_PEAK;
            pad_t[i] = now;
            pad_peak[i] = (uint16_t)r;
            break;
        case PAD_PEAK:
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
/* A scanned encoder's turns go to the live one of it and its twins: its
 * CC, mode, value and EVENT index. */
static void encoders_poll(void)
{
    for (uint32_t p = 0; p < NE_HW; p++) {
        if (enc_i2c[p])
            encoder_step(p);
        int16_t d = enc_delta[p];
        if (!d)
            continue;
        enc_delta[p] = (int16_t)(enc_delta[p] - d);
        uint32_t i = ENC_LIVE(p);
        event(EV_ENCODER, i, (uint32_t)d & 0x3FFFu);
#if GM_MCU
        if (gm_encoder_mcu[i] != GM_MCU_NONE) {
            gm_mcu_vpot(gm_encoder_mcu[i], d);
            continue;
        }
#endif
        uint32_t num = (uint32_t)gm_cfg.enc_cc[i];
        switch (gm_cfg.enc_mode[i]) {
        case ENC_REL_TWOS:
            cc(num, (uint32_t)(d > 63 ? 63 : d < -64 ? -64 : d) & 0x7F);
            break;
        case ENC_REL_OFFSET:
            cc(num, (uint32_t)(64 + (d > 63 ? 63 : d < -64 ? -64 : d)));
            break;
        default: {
            int32_t v = enc_value[i] + d;
            v = v < 0 ? 0 : v > 127 ? 127 : v;
            if (v != enc_value[i]) {
                enc_value[i] = (int16_t)v;
                cc(num, (uint32_t)v);
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
#define LV_BLINK 0x80u
#define LV_RGB 0x40u

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

static void led_show(const gm_led_t *l, uint32_t level, int blink_on)
{
#if GM_HOST_LEDS
    if (level & LV_BLINK)
        level = blink_on ? level & ~LV_BLINK : 0;
#endif
    uint32_t m;
    if (level == 0)
        m = led_look(l, 0);
    else if (!vpin_is_none(l->pin3))
        m = (level & LV_RGB) ? level & 7u : led_look(l, 1);
    else if (vpin_is_none(l->pin2) || level == 1)
        m = led_look(l, 1);
    else                            /* bicolour 2 both, 3 pin2, 4+ pin2 blinking */
        m = level == 2 ? 3u : (level == 3 || blink_on) ? 2u : led_look(l, 0);
    led_pins(l, m);
}

#if GM_HOST_LEDS
#define NL GM_ARR(GM_N_LEDS)
#define HOST_NONE 0xFFu
static uint8_t led_host_lv[NL];     /* level the host set, HOST_NONE = none */
static uint32_t led_host_t[NL];     /* when (hal_millis)                     */
#if GM_HOST_FB
static uint8_t led_host_hold[NL];   /* set by a DAW script: no timeout       */
#endif

void gm_led_host(uint32_t led, uint32_t value)
{
    if (led >= gm_device.n_leds)
        return;
    led_host_lv[led] = (uint8_t)(value == 0 ? 0 : value == 1 ? (LV_BLINK | 1u) : 1u);
    led_host_t[led] = hal_millis();
#if GM_HOST_FB
    led_host_hold[led] = 0;
#endif
}

#if GM_HOST_FB
static int bicolour(uint32_t led)
{
    return !vpin_is_none(gm_device.leds[led].pin2) && vpin_is_none(gm_device.leds[led].pin3);
}

static void led_script_set(uint32_t led, uint32_t level)
{
    led_host_lv[led] = (uint8_t)level;
    led_host_t[led] = hal_millis();
    led_host_hold[led] = 1;
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
        /* on/off per channel: lit if at least half the brightest one */
        if (m)
            lv = LV_RGB | (2u * r >= m) | (2u * g >= m) << 1 | (2u * b >= m) << 2;
    } else if (m) {
        lv = bicolour(led) && g > r ? 3u : 1u;
    }
    led_script_set(led, lv && blink ? (LV_BLINK | lv) : lv);
}
#endif

void gm_host_release(uint32_t what)
{
    if (what == 0 || what == 3)
        gm_display_host_release();
    if (what == 0 || what == 1)
        for (uint32_t i = 0; i < gm_device.n_leds; i++) {
            led_host_lv[i] = HOST_NONE;
#if GM_HOST_FB
            led_host_hold[i] = 0;
#endif
        }
#if GM_N_ANALOGS > 0 && GM_ANALOG_PICKUP
    if (what == 0 || what == 2)
        for (uint32_t i = 0; i < GM_N_ANALOGS; i++)
            a_pick[i] = -1;
#endif
}

/* Who drives LED i (core/README.md "LED ownership"). local = the device's
 * own state for it (its button's); an LED no button uses has no local
 * state, so the host always drives it. */
static uint32_t led_level(uint32_t i, uint32_t local, int has_local, uint32_t now)
{
#if GM_LED_FB
    uint32_t owner = gm_led_fb[i].owner, timeout = gm_led_fb[i].timeout;
#else
    uint32_t owner = GM_LED_OWNER, timeout = GM_LED_TIMEOUT;
#endif
    uint32_t h = led_host_lv[i];
    if (owner == GM_OWNER_LOCAL)
        return local;
    if (owner == GM_OWNER_HOST || !has_local)
        return h == HOST_NONE ? 0 : h;
#if GM_HOST_FB
    if (led_host_hold[i])
        timeout = GM_TIMEOUT_NEVER;         /* a DAW script's state stays until released */
#endif
    if (h != HOST_NONE && timeout != GM_TIMEOUT_NEVER && now - led_host_t[i] >= timeout * 10u)
        led_host_lv[i] = (uint8_t)(h = HOST_NONE);   /* host went quiet: local again */
    return h == HOST_NONE ? local : h;
}
#endif

/* What each LED last showed: (level << 1) | blink phase, 0xFFFF = not known
 * (start, after the lamp test). leds_update() runs every 10 ms over every
 * LED; an LED whose level and phase didn't change isn't written again. */
static uint16_t led_shown[GM_ARR(GM_N_LEDS)];

/* Whether level shows differently in the two blink phases (led_show). */
static int led_blinks(const gm_led_t *l, uint32_t level)
{
#if GM_HOST_LEDS
    if (level & LV_BLINK)
        return 1;
#endif
    return !vpin_is_none(l->pin2) && vpin_is_none(l->pin3) && level >= 4u;
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
    /* The blink phase is only part of the key for a blinking level: a
     * steady LED isn't written again every 500 ms. */
    int ph = led_blinks(&gm_device.leds[i], level) && blink_on;
    uint16_t key = (uint16_t)(((level & 0x7FFFu) << 1) | (ph ? 1u : 0u));
    if (led_shown[i] == key)
        return;
    led_shown[i] = key;
    led_show(&gm_device.leds[i], level, blink_on);
}

#if GM_LAYERS
/* Whether a twin of this layer (mode index + 1, 0 = not a twin) takes over
 * in this pass: pass 0 the toggled mode's layer (its root's, from a
 * sub-mode), pass 1 the held hold mode's, so that one wins. */
static int twin_pass(uint32_t layer, uint32_t pass)
{
    uint32_t m = layer - 1u;
    if (!layer)
        return 0;
    if (pass)
        return hold_cur != GM_MODE_NONE && hold_cur == m;
    return hold_cur != m && mode_cur != GM_MODE_NONE && gm_modes[mode_cur].root == m;
}

/* Which of each base and its twins is live (core/README.md "Button layers"):
 * the twin of an active layer, the held hold mode's first; else the base.
 * An LED that changes hands is drawn again. */
static void layers_update(void)
{
#if GM_PAD_TWINS
    for (uint32_t i = 0; i < GM_N_PADS; i++)
        pad_live[i] = 0;
#endif
#if GM_ENC_TWINS
    for (uint32_t i = 0; i < NE_HW; i++)
        enc_live[i] = (uint8_t)i;
#endif
    uint64_t live = 0;
    for (uint32_t i = 0; i < GM_N_BUTTONS && i < 64; i++)
        if (!gm_device.buttons[i].layer)
            live |= 1ull << i;
    for (uint32_t pass = 0; pass < 2; pass++) {     /* toggled, then held: the hold mode wins */
#if GM_PAD_TWINS
        for (uint32_t t = 0; t < GM_PAD_TWINS; t++)
            if (twin_pass(gm_pad_twins[t].layer, pass))
                pad_live[gm_pad_twins[t].base] = (uint8_t)(t + 1u);
#endif
#if GM_ENC_TWINS
        for (uint32_t i = NE_HW; i < GM_N_ENCODERS; i++)
            if (twin_pass(gm_device.encoders[i].layer, pass))
                enc_live[gm_device.encoders[i].base] = (uint8_t)i;
#endif
        for (uint32_t i = 0; i < GM_N_BUTTONS && i < 64; i++) {
            const gm_button_t *b = &gm_device.buttons[i];
            if (!twin_pass(b->layer, pass))
                continue;
            for (uint32_t j = 0; j < GM_N_BUTTONS && j < 64; j++)
                if (j == b->base || (gm_device.buttons[j].layer && gm_device.buttons[j].base == b->base))
                    live &= ~(1ull << j);
            live |= 1ull << i;
        }
    }
    btn_live = live;
    uint8_t on[GM_ARR(GM_N_LEDS)];
    for (uint32_t i = 0; i < GM_N_LEDS; i++)
        on[i] = !gm_device.leds[i].layer;
    for (uint32_t pass = 0; pass < 2; pass++)
        for (uint32_t i = 0; i < GM_N_LEDS; i++) {
            const gm_led_t *l = &gm_device.leds[i];
            if (!twin_pass(l->layer, pass))
                continue;
            for (uint32_t j = 0; j < GM_N_LEDS; j++)
                if (j == l->base || (gm_device.leds[j].layer && gm_device.leds[j].base == l->base))
                    on[j] = 0;
            on[i] = 1;
        }
    for (uint32_t i = 0; i < GM_N_LEDS; i++)
        if (led_dead[i] != !on[i]) {
            led_dead[i] = !on[i];
            led_shown[i] = 0xFFFFu;     /* drawn again once it is live */
        }
}
#endif

#if GM_HOST_LEDS
/* leds_update() works out the buttons' and pads' own levels every 10 ms;
 * leds_apply() then shows LED_STEP LEDs per main-loop pass, so no one pass
 * carries all of them. */
#define LED_STEP 13u
static uint8_t local[NL], has_local[NL];
static uint8_t led_next = 0xFFu;    /* next LED to show, 0xFF = none */
static uint32_t led_now;
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

static void leds_update(uint32_t now)
{
#if GM_LED_WALK_MS
    leds_walk(now);
    return;
#endif
    int blink_on = (now / 500u) & 1u;
#if GM_HOST_LEDS
    for (uint32_t i = 0; i < NL; i++)
        local[i] = has_local[i] = 0;
#endif
    for (uint32_t i = 0; i < gm_device.n_buttons; i++) {
        int8_t li = gm_device.buttons[i].led;
        if (li < 0 || li >= (int8_t)gm_device.n_leds)
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
        local[li] = (uint8_t)level;
        has_local[li] = 1;
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
        if (li < 0 || li >= (int8_t)gm_device.n_leds)
            continue;
        uint32_t level = PAD_OWNER(i) == owner && (pad_state[i] == PAD_ON ||
                         (pad_lit[i] && now - pad_lit[i] < PAD_LED_MIN_MS));
#if GM_HOST_LEDS
        local[li] = (uint8_t)level;
        has_local[li] = 1;
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
        led_set(i, MODE_LED(i, led_level(i, local[i], has_local[i], now), now), blink_on);
    led_next = end >= gm_device.n_leds ? 0xFFu : (uint8_t)end;
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

#if GM_LED_FB
/* The host's value for every LED with this explicit source. */
static void leds_from(uint32_t source, uint32_t num, uint32_t ch, uint32_t value)
{
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
    if (ch != chan())
        return;
#if GM_N_BUTTONS > 0
    for (uint32_t i = 0; i < GM_N_BUTTONS; i++) {
        if ((uint32_t)gm_cfg.btn_number[i] != num)
            continue;
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
#if GM_N_ANALOGS > 0
    for (uint32_t i = 0; i < GM_N_ANALOGS; i++) {
#if GM_MCU
        if (gm_analog_mcu[i] != GM_MCU_NONE)
            continue;                       /* an MCU fader follows pitch bend */
#endif
        if (gm_cfg.analog_mode[i] == AN_MODE_CC && (uint32_t)gm_cfg.analog_cc[i] == num)
            gm_analog_host(i, v);
    }
#endif
#if GM_N_ENCODERS > 0
    for (uint32_t i = 0; i < GM_N_ENCODERS; i++)
        if ((uint32_t)gm_cfg.enc_cc[i] == num && gm_cfg.enc_mode[i] == ENC_ABSOLUTE)
            enc_value[i] = (int16_t)v;      /* absolute: carry on from the host's value */
#endif
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
            cc((uint32_t)gm_cfg.btn_number[i], 0);
#if GM_MCU
        if (held && act == ACT_MCU)
            gm_mcu_note((uint32_t)gm_cfg.btn_number[i], 0);
#endif
        if (act == ACT_CC_RAMP && ramp_sent[i] != 0)
            cc((uint32_t)gm_cfg.btn_number[i], 0);
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
    for (uint32_t i = 0; i < gm_device.n_hubs; i++) {
        uint8_t t = gm_device.hubs[i].type;
        if (t == GM_HUB_HC595 || t == GM_HUB_HC165)
            has_sr_hubs = 1;
        if (t == GM_HUB_MCP23017 || t == GM_HUB_PCF8574)
            has_i2c_hubs = 1;
        if (t == GM_HUB_SWMATRIX)
            has_matrices = 1;
        if (t == GM_HUB_TM1628)
            has_tm_hubs = 1;
        if (t == GM_HUB_RGB_SERIAL)
            has_rgb_hubs = 1;
    }
    vpin_init();
    gm_din_init();
    for (uint32_t i = 0; i < gm_device.n_buttons; i++)
        vpin_input(gm_device.buttons[i].pin);
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
        rows_dec = m->n_rows > 0;
        rows_sel = m->n_rows > 0 && m->n_rows <= FAST_ROWS;
        for (uint32_t r = 0; r < m->n_rows; r++) {
            vpin_output(m->rows[r]);
            if (!vpin_is_decoder(m->rows[r]))
                rows_dec = 0;
            if (r >= FAST_ROWS || !vpin_select_word(m->rows[r], &row_sel_reg[r], &row_sel_word[r]))
                rows_sel = 0;
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
            } else {
                col_run[n_runs++] = (col_run_t){ reg, 1u, bit, (uint8_t)c };
            }
            if (m->cols[c].flags & GM_PIN_INVERT)
                col_inv |= 1u << c;
        }
        for (uint32_t k = 0; k < NK; k++)
            knote[k] = NOTE_NONE;
    }
#endif
#if GM_N_MODES > 0
    mode_cur = hold_cur = GM_MODE_NONE;
    show_n = 0;
    btn_in_mode = 0;
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
#if GM_MCU
    gm_mcu_init(fw_version);
#endif

    gm_settings_init();
    presets_boot(safe);
    app_log_limit = (uint8_t)gm_cfg.log_level;
    app_proto_init(fw_version, 1);

    /* Lamp test: every LED on briefly. */
    for (uint32_t i = 0; i < gm_device.n_leds; i++)
        led_pins(&gm_device.leds[i], 7u);
    vpin_flush();
    vpin_flush_i2c();
    vpin_flush_tm1628();
    hal_delay_us(200000);
    for (uint32_t i = 0; i < gm_device.n_leds; i++) {
        led_pins(&gm_device.leds[i], 0);
        led_shown[i] = 0xFFFFu;         /* leds_update writes each once (its idle look, if off) */
    }
    vpin_flush();
    vpin_flush_i2c();
    vpin_flush_tm1628();
}

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

    app_proto_poll();
    gm_din_poll(now);
    presets_poll(now);
    app_log_limit = (uint8_t)gm_cfg.log_level;
    poll_part_done(0, &tc);

    if (has_i2c_hubs && now - t_i2c >= I2C_MS) {
        t_i2c = now;
        vpin_flush_i2c();
        vpin_refresh_i2c();
    }
#if GM_HAS_MATRIX
    keys_poll();
#endif
#if GM_N_BUTTONS > 0
    if (now - t_btn >= BTN_SAMPLE_MS) {
        t_btn = now;
        buttons_sample();
    }
    if (now - t_ms > 50u)
        t_ms = now - 1u;            /* first call, or a long stall (flash erase) */
    while (t_ms != now) {
        t_ms++;
        ramps_ms(now);
    }
#endif
    poll_part_done(1, &tc);
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
    encoders_poll();
#endif
    poll_part_done(4, &tc);
    int led_pass = 0;
    if (now - t_led >= LED_MS) {
        t_led = now;
        leds_update(now);           /* the buttons' and pads' levels */
        gm_display_poll(now);       /* the display's segments: written with the LEDs */
        led_pass = 1;
    } else {
        leds_apply();               /* then the LEDs, a few per pass */
    }
    poll_part_done(5, &tc);
    /* Changed TM1628 bytes, a few per pass, and not on the LED update's
     * pass, so the two never add up in one pass. */
    if (has_tm_hubs && !led_pass)
        vpin_poll_tm1628();
    if (has_rgb_hubs)
        vpin_poll_rgb_serial(now);
    poll_part_done(6, &tc);
    if (!booted && now >= BOOT_LOG_MS) {
        booted = 1;
        app_log(LOG_INFO, gm_device.name, safe ? " up (safe mode: defaults)" : " up");
        app_log_hex(LOG_INFO, "fw ", fw_ver);
#if GM_N_PADS > 0
        pads_report();
#endif
    }
    hal_midi_poll();
}
