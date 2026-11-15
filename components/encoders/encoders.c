/*
 * encoders.c: the rotary encoders (encoders:), a control component
 * (README.md here). The scan (core/src/controller.c gm_scan_tick, and the
 * hardware scan's frames) calls encoder_step, which counts detents; the
 * main loop (gm_poll) calls encoders_poll, which sends them. Shares its
 * state through gm_controls.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_controls.h"
#include "gm_device.h"
#include "gm_settings.h"
#include "gm_feedback.h"
#include "gm_mcu.h"
#include "gm_vpin.h"

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
volatile uint16_t enc_count[GM_ARR(NE_HW)];
static uint16_t enc_seen[GM_ARR(NE_HW)];
uint8_t enc_i2c[GM_ARR(NE_HW)];
int16_t enc_value[NE];
/* One-load reads of A and B (vpin_in_reg), electrical levels; 0 = vpin_read. */
static const volatile uint32_t *enc_ra[GM_ARR(NE_HW)], *enc_rb[GM_ARR(NE_HW)];
static uint32_t enc_ma[GM_ARR(NE_HW)], enc_mb[GM_ARR(NE_HW)];
#if GM_ENC_TWINS
uint8_t enc_live[GM_ARR(NE_HW)];
#endif

#if GM_MSG_KINDS
_Static_assert(ENC_CC14 - 3 == K_CC14 && ENC_NRPN - 3 == K_NRPN && ENC_RPN14 - 3 == K_RPN14, "enc_kind");
/* What encoder.mode sends: K_CC for the 7-bit modes, the K_* of the
 * absolute ones from ENC_CC14 on. */
uint32_t enc_kind(int32_t mode)
{
    return mode >= ENC_CC14 ? (uint32_t)mode - 3u : K_CC;
}
#endif

/* The top of encoder i's values: 127, or 16383 in a 14-bit mode. */
int32_t enc_top(uint32_t i)
{
#if GM_MSG_KINDS
    if (kind_wide(enc_kind(gm_cfg.enc_mode[i]), (uint32_t)gm_cfg.enc_cc[i]))
        return 16383;
#else
    (void)i;
#endif
    return 127;
}

/* In an absolute mode, enc_value is the position 0..span along min..max
 * (0..top without them); 0: a relative mode. */
int32_t enc_span(uint32_t i, int32_t top)
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
uint32_t enc_out(uint32_t i, int32_t p, int32_t top)
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

void enc_at(uint32_t i, int32_t v, int32_t top, int32_t span)
{
#if GM_ENC_CTL
    const gm_ctl_t *t = &gm_encoder_ctl[i];
    v = ctl_hi(t, top) >= t->min ? v - t->min : t->min - v;
#else
    (void)top;
#endif
    enc_value[i] = (int16_t)(v < 0 ? 0 : v > span ? span : v);
}

/* FB_VALUE: value 0..16383 over the encoder's range. */
void gm_encoder_host(uint32_t i, uint32_t value)
{
    if (i >= GM_N_ENCODERS)
        return;
    int32_t span = enc_span(i, enc_top(i));
    if (span == 127)
        enc_value[i] = (int16_t)(value >> 7);
    else if (span)
        enc_value[i] = (int16_t)((int32_t)value * span / 16383);
}

void encoder_step(uint32_t i)
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

#if GM_ENC_ACCEL
/* Acceleration (components/encoders/README.md "Encoder acceleration"), per scanned encoder
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
void encoders_poll(uint32_t now)
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
        send_event(EV_ENCODER, i, (uint32_t)d & 0x3FFFu);
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
                send_value(enc_kind(gm_cfg.enc_mode[i]), ENC_CH(i), num, enc_out(i, v, top));
#else
                midi3(st, num, enc_out(i, v, top));
#endif
            }
            break;
        }
        }
    }
}

void encoders_init(void)
{
    for (uint32_t i = 0; i < NE_HW; i++) {
        const gm_encoder_t *e = &gm_device.encoders[i];
        vpin_input(e->a);
        vpin_input(e->b);
        enc_i2c[i] = (uint8_t)(vpin_on_i2c(e->a) || vpin_on_i2c(e->b));
        enc_ra[i] = vpin_in_reg(e->a, &enc_ma[i]);
        enc_rb[i] = vpin_in_reg(e->b, &enc_mb[i]);
    }
}
#endif
