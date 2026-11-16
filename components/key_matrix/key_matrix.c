/*
 * key_matrix.c: the key matrix (key_matrix:), a control component
 * (README.md here): scanning it, timing a velocity key's two contacts, and
 * the key queue to the main loop, which plays the notes (keys_poll). The
 * scan (core/src/controller.c gm_scan_tick, the scan interrupt) calls
 * matrix_scan, or with the hardware scan gm_scan_frames hands it each
 * frame's key rows (matrix_frame). Shares its state through gm_controls.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_controls.h"
#include "gm_controller.h"
#include "gm_device.h"
#include "gm_settings.h"
#include "gm_vpin.h"
#include "gm_hal.h"

#if GM_HAS_MATRIX
/* The scan tables' sizes, from the builder (core/README.md "Generated
 * code"); a config generated before them gets the most. */
#ifndef GM_MATRIX_COL_RUNS
#define GM_MATRIX_COL_RUNS 16       /* col_run entries matrix_init makes */
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
uint32_t mprev[FAST_ROWS];
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
 * no shift-register hubs, contacts on columns (the Keystation).
 * matrix_init picks it (scan_fast_init). */
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
/* Contacts on rows 2p and 2p+1 (a velocity matrix with contacts: rows, the
 * Code 61): the scans take rows in pairs, and there's no fast scan. Of the
 * const config, so the test folds to a constant (a cached copy is a load
 * more in the scan tick). */
static int contacts_on_rows(const gm_matrix_t *m)
{
    return m->kind == GM_MATRIX_KIND_VELOCITY && m->contacts == GM_MATRIX_CONTACTS_ROWS;
}
static int by_rows_pairs;           /* contacts_on_rows, for the hardware scan's frames (matrix_init) */
#if GM_WRITE_LAST_PRESET
uint8_t keys_down;
#define KEY_HELD(on) (keys_down = (uint8_t)((on) ? keys_down + 1u : keys_down ? keys_down - 1u : 0u))
#else
#define KEY_HELD(on) ((void)0)
#endif

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
            kt0[k] = (uint16_t)scan_ticks;
        }
        break;
    case K_TIMING:
        if (c == 3u) {
            if (!kq_push(k, 1, (uint16_t)(scan_ticks - kt0[k])))
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

/* One key per scan tick (with the hardware scan: per batch of frames): a
 * key still timing after KT0_MAX ticks stays at KT0_MAX (seconds: velocity
 * 1 long before). Each key is seen every NK ticks (batches of a few frames),
 * so its age stays far below 0x10000. */
void kt0_age(void)
{
    uint32_t k = kt0_next;
    kt0_next = (uint8_t)(k + 1u < NK ? k + 1u : 0u);
    if (kstate[k] == K_TIMING && (uint16_t)(scan_ticks - kt0[k]) >= KT0_MAX)
        kt0[k] = (uint16_t)(scan_ticks - KT0_MAX);
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

/* matrix_init, once the rows and columns are set up. (Not inlined: smaller
 * that way.) */
__attribute__((noinline)) static void scan_fast_init(const gm_matrix_t *m)
{
    scan_fast = rows_fast && cols_fast && !m_hubs && n_runs && !contacts_on_rows(m);
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

void matrix_scan(void)
{
    const gm_matrix_t *m = gm_device.matrix;
#if SCAN_FAST
    if (scan_fast) {
        matrix_scan_fast(m);
        return;
    }
#endif
    uint32_t rows = m->n_rows > FAST_ROWS ? FAST_ROWS : m->n_rows;
    int by_rows = contacts_on_rows(m);
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
    /* The key time in us (the tick period q4 is 1/16 us): (dt * q4) >> 4
     * as two products that fit 32 bits (dt <= 0xFFFF, q4 < 2^20), no
     * divide; 2^28 us or more saturates. */
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

void keys_poll(void)
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
            kchan[e.key] = midi_chan();
            midi3(0x90 | kchan[e.key], (uint32_t)n, v);
            send_event(EV_KEY, e.key, v);
        } else {
            if (knote[e.key] != NOTE_NONE)
                midi3(0x80 | kchan[e.key], knote[e.key], 0);
            knote[e.key] = NOTE_NONE;
            send_event(EV_KEY, e.key, 0);
        }
    }
}

/* Note Off for every sounding key; their releases then send nothing. */
void keys_off(void)
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

/* gm_init, with a key matrix: its pins, and the scan's fast paths. */
void matrix_init(void)
{
    const gm_matrix_t *m = gm_device.matrix;
    by_rows_pairs = contacts_on_rows(m);
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

#if GM_HW_SCAN
int matrix_plan(gm_scan_plan_t *plan, uint32_t max_rows)
{
    const gm_matrix_t *m = gm_device.matrix;
    if (!m)
        return 1;
    if (!rows_sel || !cols_fast || m_hubs || m->n_rows > max_rows) {
        app_log(LOG_WARN, "hardware scan: key matrix rows need one-store selects and ",
                "columns on one input register; scanning in software");
        return 0;
    }
#if GM_MATRIX_DECODER
    for (uint32_t r = 0; r < m->n_rows; r++) {
        if (plan->sel_reg && row_sel_reg[r] != plan->sel_reg)
            return -1;
        plan->sel_reg = row_sel_reg[r];
        plan->sel_word[r] = row_sel_word[r];
    }
#endif
    for (uint32_t i = 0; i < n_runs; i++) {
        if (plan->in_reg && col_run[i].reg != plan->in_reg)
            return -1;
        plan->in_reg = col_run[i].reg;
        plan->in_mask |= col_run[i].mask << col_run[i].src;
    }
    plan->n_rows = m->n_rows;
    return 1;
}

uint32_t matrix_retry(uint32_t key_rows)
{
    if (!kq_retry)
        return 0;
    kq_retry = 0;
    return (1u << ((key_rows + 1u) / 2u)) - 1u;
}

void matrix_frame(const uint16_t *raw, uint32_t chg, uint32_t key_rows)
{
    const gm_matrix_t *m = gm_device.matrix;
    for (uint32_t k = 0; m && 2u * k < key_rows; k++) {
        if (!((chg >> k) & 1u))
            continue;
        uint32_t even = 0;
        matrix_row(m, 2u * k, cols_of(raw[2u * k]), key_rows, by_rows_pairs, &even);
        if (2u * k + 1u < key_rows)
            matrix_row(m, 2u * k + 1u, cols_of(raw[2u * k + 1u]), key_rows, by_rows_pairs, &even);
    }
}
#endif
#endif
