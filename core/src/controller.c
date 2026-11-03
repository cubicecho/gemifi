/*
 * controller.c: the generic MIDI controller runtime. See gm_controller.h
 * and core/README.md for what the components and actions do.
 *
 * Split between contexts:
 *   gm_scan_tick() (timer interrupt): key matrix, encoders not on I2C hubs,
 *     shift-register hub refresh/flush. Key changes go to the main loop
 *     through a small queue.
 *   gm_poll() (main loop): everything that sends MIDI, buttons, analog
 *     inputs, ramps, LEDs, I2C hubs, the protocol.
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

#define BTN_SAMPLE_MS   5u
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
#if GM_N_MODES > 0 && (!GM_HAS_MATRIX || GM_N_BUTTONS == 0)
#error "modes need a key matrix and a toggle button (the builder checks this)"
#endif

static uint32_t fw_ver;
static uint8_t has_sr_hubs;         /* any 74HC595/165 hub: refresh in the tick */
static uint8_t has_i2c_hubs;

/* ---- MIDI out --------------------------------------------------------- */

static uint8_t chan(void)
{
    return (uint8_t)((gm_cfg.channel - 1) & 0x0F);
}

static void midi3(uint8_t status, uint32_t a, uint32_t b)
{
    uint8_t m[3] = { status, (uint8_t)(a & 0x7F), (uint8_t)(b & 0x7F) };
    hal_midi_send(m, 3);
}

#if GM_N_BUTTONS > 0
static void midi2(uint8_t status, uint32_t a)
{
    uint8_t m[2] = { status, (uint8_t)(a & 0x7F) };
    hal_midi_send(m, 2);
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
static uint8_t m_hubs;              /* rows or cols on a hub */
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
static uint8_t knote[NK], kchan[NK];
#endif

#if GM_N_MODES > 0
static uint8_t mode_cur = GM_MODE_NONE;     /* the active mode; GM_MODE_NONE = keys play */
static void mode_key(uint32_t k);
#endif

static volatile uint32_t ticks;
volatile uint32_t gm_scan_period_us;

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

static void matrix_scan(void)
{
    const gm_matrix_t *m = gm_device.matrix;
    uint32_t rows = m->n_rows > 32 ? 32 : m->n_rows;
    for (uint32_t r = 0; r < rows; r++) {
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
        else
            vpin_write(m->rows[r], 0);
        uint32_t diff = s ^ mprev[r];
        if (!diff)
            continue;
        mprev[r] = s;
        if (m->kind == GM_MATRIX_KIND_VELOCITY) {
            for (uint32_t p = 0; 2u * p + 1u < m->n_cols; p++) {
                if (!((diff >> (2u * p)) & 3u))
                    continue;
                uint32_t k = r + rows * p;
                if (k < m->n_keys)
                    key_update(k, (s >> (2u * p)) & 3u);
            }
        } else {
            for (uint32_t c = 0; c < m->n_cols; c++) {
                if (!((diff >> c) & 1u))
                    continue;
                uint32_t k = r + rows * c;
                if (k < m->n_keys)
                    kq_push(k, (s >> c) & 1u, 0);
            }
        }
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
    uint32_t ab = (uint32_t)(vpin_read(e->a) << 1) | (uint32_t)vpin_read(e->b);
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

void gm_scan_tick(void)
{
    ticks++;
    if (has_sr_hubs)
        vpin_refresh();
#if GM_HAS_MATRIX
    if (gm_device.matrix)
        matrix_scan();
#endif
#if GM_N_ENCODERS > 0
    for (uint32_t i = 0; i < GM_N_ENCODERS; i++)
        if (!enc_i2c[i])
            encoder_step(i);
#endif
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
    uint32_t t = dt_ticks * gm_scan_period_us / 100u;       /* 0.1 ms */
    int32_t fast = gm_cfg.vel_fast, slow = gm_cfg.vel_slow;
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
            if (mode_cur != GM_MODE_NONE) {
                knote[e.key] = NOTE_NONE;   /* a function key: no note, its release sends nothing */
                mode_key(e.key);
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
static uint32_t btn_stable, btn_last, btn_toggled;
static uint32_t btn_ignore;         /* held through a preset switch: release is silent */
static int32_t ramp_q8[NB];         /* CC ramp position, 0..127 << 8 */
static int16_t ramp_sent[NB];
static uint8_t btn_note[NB], btn_chan[NB];
static int32_t bend, bend_target, bend_sent = 0x7FFFFFFF;
static int32_t program;

static uint32_t read_buttons(void)
{
    uint32_t s = 0;
    for (uint32_t i = 0; i < GM_N_BUTTONS && i < 32; i++)
        if (vpin_read(gm_device.buttons[i].pin))
            s |= 1u << i;
    return s;
}

static int held_with_action(int32_t act, uint32_t except)
{
    for (uint32_t i = 0; i < GM_N_BUTTONS; i++)
        if (i != except && ((btn_stable >> i) & 1u) && gm_cfg.btn_action[i] == act)
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
            btn_toggled ^= 1u << i;
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

/* The toggle button of a root mode: enters the mode (held keys get their
 * Note Off first), or leaves it from the mode or any of its sub-modes. Its
 * own action never runs. Returns 0 if button i toggles no mode. */
static int mode_button(uint32_t i, int down)
{
    for (uint32_t m = 0; m < GM_N_MODES; m++) {
        if (gm_modes[m].toggle != i)
            continue;
        event(EV_BUTTON, i, (uint32_t)down);
        if (!down)
            return 1;
        if (mode_cur != GM_MODE_NONE && gm_modes[mode_cur].root == m) {
            mode_cur = GM_MODE_NONE;
        } else {
            if (mode_cur == GM_MODE_NONE)
                keys_off();
            mode_cur = (uint8_t)m;
        }
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

/* A key pressed in the active mode: run its binding (unbound keys do
 * nothing), then a sub-mode goes back to its parent. */
static void mode_key(uint32_t k)
{
    const gm_mode_t *m = &gm_modes[mode_cur];
    const gm_mode_key_t *b = 0;
    for (uint32_t i = 0; i < m->n_keys; i++)
        if (m->keys[i].key == k) {
            b = &m->keys[i];
            break;
        }
    if (!b)
        return;
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
        show_mode = mode_cur;
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
        set_live(&gm_cfg.channel, b->arg, P_CHANNEL);
        break;
    case GM_MK_CURVE:
        set_live(&gm_cfg.vel_curve, b->arg, P_VEL_CURVE);
        break;
    default:
        break;
    }
    if (m->parent != GM_MODE_NONE)
        mode_cur = m->parent;
}

/* What LED li shows instead of level: after a preset switch from a mode,
 * its count LED blinks the slot number, then it and the done LED flash
 * once; while a mode is active, its LED is lit (or blinks). */
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
    if (mode_cur != GM_MODE_NONE && gm_modes[mode_cur].led == (int8_t)li)
        return gm_modes[mode_cur].led_blink ? (now / MODE_BLINK_MS) & 1u : 1u;
    return level;
}
#define MODE_LED(li, level, now) mode_led(li, level, now)
#endif

static void buttons_sample(void)
{
    uint32_t s = read_buttons();
    if (s == btn_last && s != btn_stable) {
        uint32_t diff = s ^ btn_stable;
        btn_stable = s;
        for (uint32_t i = 0; i < GM_N_BUTTONS && i < 32; i++) {
            if (!((diff >> i) & 1u))
                continue;
            if ((btn_ignore >> i) & 1u) {
                btn_ignore &= ~(1u << i);   /* released after a preset switch */
                continue;
            }
#if GM_N_MODES > 0
            if (mode_button(i, (s >> i) & 1u))
                continue;
#endif
            button_changed(i, (s >> i) & 1u);
        }
    }
    btn_last = s;
}

/* Called once per elapsed millisecond. */
static void ramps_ms(uint32_t now)
{
    for (uint32_t i = 0; i < GM_N_BUTTONS; i++) {
        if (gm_cfg.btn_action[i] != ACT_CC_RAMP)
            continue;
        int32_t target = ((btn_stable >> i) & 1u) ? (127 << 8) : 0;
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

#if GM_N_ANALOGS > 0
static int32_t a_filt[NA];          /* 0..4095 << 2 */
static int16_t a_sent[NA];          /* last 7-bit value, -1 = none yet */
static uint8_t a_next;
#if GM_ANALOG_PICKUP
/* Pick-up: after the host sets a value the knob isn't at, the knob stays
 * silent until it reaches or crosses that value. */
static int8_t a_pick[NA];           /* host value to reach, -1 = in control */
static int8_t a_side[NA];           /* side of a_pick the knob was on: -1 / 1 */
#endif

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

static void analog_poll(void)
{
    uint32_t i = a_next;
    a_next = (uint8_t)((a_next + 1u) % GM_N_ANALOGS);
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
    if (a_sent[i] < 0) {
        a_filt[i] = v << 2;                 /* first reading: no message */
        a_sent[i] = (int16_t)(v >> 5);
        return;
    }
    a_filt[i] += v - (a_filt[i] >> 2);     /* 4-sample moving average */
    int32_t pos = a_filt[i] >> 2;           /* 0..4095 */
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
                cc((uint32_t)gm_cfg.analog_cc[i], (uint32_t)q);
            event(EV_ANALOG, i, (uint32_t)pos << 2);
        }
    }
}
#endif

/* ---- encoders (main side) --------------------------------------------- */

#if GM_N_ENCODERS > 0
static void encoders_poll(void)
{
    for (uint32_t i = 0; i < GM_N_ENCODERS; i++) {
        if (enc_i2c[i])
            encoder_step(i);
        int16_t d = enc_delta[i];
        if (!d)
            continue;
        enc_delta[i] = (int16_t)(enc_delta[i] - d);
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
 * (bicolour). LV_BLINK | level: that level, blinking (host feedback). */
#define LV_BLINK 0x80u

static void led_show(const gm_led_t *l, uint32_t level, int blink_on)
{
#if GM_HOST_LEDS
    if (level & LV_BLINK)
        level = blink_on ? level & ~LV_BLINK : 0;
#endif
    if (vpin_is_none(l->pin2)) {
        vpin_write(l->pin, level > 0);
        return;
    }
    vpin_write(l->pin, level == 1 || level == 2);
    vpin_write(l->pin2, level == 2 || level == 3 || (level >= 4 && blink_on));
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
    return !vpin_is_none(gm_device.leds[led].pin2);
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
    uint32_t lv = 0;
    if (r || g || b)
        lv = bicolour(led) && g > r ? 3u : 1u;
    led_script_set(led, lv && blink ? (LV_BLINK | lv) : lv);
}
#endif

void gm_host_release(uint32_t what)
{
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

static void leds_update(uint32_t now)
{
    int blink_on = (now / 500u) & 1u;
#if GM_HOST_LEDS
    uint8_t local[NL], has_local[NL];
    for (uint32_t i = 0; i < NL; i++)
        local[i] = has_local[i] = 0;
#endif
    for (uint32_t i = 0; i < gm_device.n_buttons; i++) {
        int8_t li = gm_device.buttons[i].led;
        if (li < 0 || li >= (int8_t)gm_device.n_leds)
            continue;
        uint32_t level = 0;
#if GM_N_BUTTONS > 0
        int held = (btn_stable >> i) & 1u;
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
        led_show(&gm_device.leds[li], MODE_LED((uint32_t)li, level, now), blink_on);
#endif
    }
#if GM_N_MODES > 0 && !GM_HOST_LEDS
    for (uint32_t i = 0; i < gm_device.n_leds; i++) {
        uint32_t lv = mode_led(i, 0xFFu, now);    /* also LEDs no button drives */
        if (lv != 0xFFu)
            led_show(&gm_device.leds[i], lv, blink_on);
    }
#endif
#if GM_HOST_LEDS
    for (uint32_t i = 0; i < gm_device.n_leds; i++)
        led_show(&gm_device.leds[i], MODE_LED(i, led_level(i, local[i], has_local[i], now), now), blink_on);
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
                btn_toggled |= 1u << i;
            else
                btn_toggled &= ~(1u << i);
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
        if ((uint32_t)gm_cfg.analog_cc[i] == num)
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
    for (uint32_t i = 0; i < GM_N_BUTTONS && i < 32; i++) {
        int held = (btn_stable >> i) & 1u;
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
    btn_ignore = btn_stable;
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
    }
    vpin_init();
    for (uint32_t i = 0; i < gm_device.n_buttons; i++)
        vpin_input(gm_device.buttons[i].pin);
    for (uint32_t i = 0; i < gm_device.n_leds; i++) {
        vpin_output(gm_device.leds[i].pin);
        if (!vpin_is_none(gm_device.leds[i].pin2))
            vpin_output(gm_device.leds[i].pin2);
    }
#if GM_N_ENCODERS > 0
    for (uint32_t i = 0; i < GM_N_ENCODERS; i++) {
        const gm_encoder_t *e = &gm_device.encoders[i];
        vpin_input(e->a);
        vpin_input(e->b);
        enc_i2c[i] = (uint8_t)(vpin_on_i2c(e->a) || vpin_on_i2c(e->b));
    }
#endif
#if GM_HAS_MATRIX
    if (gm_device.matrix) {
        const gm_matrix_t *m = gm_device.matrix;
        rows_fast = m->n_rows <= FAST_ROWS;
        for (uint32_t r = 0; r < m->n_rows; r++) {
            vpin_output(m->rows[r]);
            if (m->rows[r].hub)
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
            if (m->cols[c].hub)
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
    mode_cur = GM_MODE_NONE;
    show_n = 0;
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
        led_show(&gm_device.leds[i], 2, 1);
    vpin_flush();
    vpin_flush_i2c();
    hal_delay_us(200000);
    for (uint32_t i = 0; i < gm_device.n_leds; i++)
        led_show(&gm_device.leds[i], 0, 0);
    vpin_flush();
    vpin_flush_i2c();
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
    static uint8_t booted;
    uint32_t now = hal_millis();

    app_proto_poll();
    presets_poll(now);
    app_log_limit = (uint8_t)gm_cfg.log_level;

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
#if GM_N_ANALOGS > 0
    if (now - t_analog >= ANALOG_MS) {
        t_analog = now;
        analog_poll();
    }
#endif
#if GM_N_ENCODERS > 0
    encoders_poll();
#endif
    if (now - t_led >= LED_MS) {
        t_led = now;
        leds_update(now);
    }
    if (!booted && now >= BOOT_LOG_MS) {
        booted = 1;
        app_log(LOG_INFO, gm_device.name, safe ? " up (safe mode: defaults)" : " up");
        app_log_hex(LOG_INFO, "fw ", fw_ver);
    }
    hal_midi_poll();
}
