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
#include "config_store.h"

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
#endif

/* ---- buttons ----------------------------------------------------------- */

#if GM_N_BUTTONS > 0
static uint32_t btn_stable, btn_last, btn_toggled;
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
    app_log(LOG_DEBUG, gm_device.buttons[i].label, down ? " down" : " up");
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
    default:                        /* ACT_NONE, ACT_CC_RAMP (in ramps_ms) */
        break;
    }
}

static void buttons_sample(void)
{
    uint32_t s = read_buttons();
    if (s == btn_last && s != btn_stable) {
        uint32_t diff = s ^ btn_stable;
        btn_stable = s;
        for (uint32_t i = 0; i < GM_N_BUTTONS && i < 32; i++)
            if ((diff >> i) & 1u)
                button_changed(i, (s >> i) & 1u);
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

static void led_show(const gm_led_t *l, uint32_t level, int blink_on)
{
    if (vpin_is_none(l->pin2)) {
        vpin_write(l->pin, level > 0);
        return;
    }
    vpin_write(l->pin, level == 1 || level == 2);
    vpin_write(l->pin2, level == 2 || level == 3 || (level >= 4 && blink_on));
}

static void leds_update(uint32_t now)
{
    int blink_on = (now / 500u) & 1u;
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
        led_show(&gm_device.leds[li], level, blink_on);
    }
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
#if GM_N_BUTTONS > 0
    for (uint32_t i = 0; i < NB; i++) {
        btn_note[i] = NOTE_NONE;
        ramp_sent[i] = 0;
    }
#endif
#if GM_N_ANALOGS > 0
    for (uint32_t i = 0; i < NA; i++)
        a_sent[i] = -1;
#endif

    gm_settings_init();
    params_defaults();
    if (!safe)
        config_store_load();
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

void gm_midi_rx_byte(uint8_t b)
{
    app_proto_rx_byte(b);
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
