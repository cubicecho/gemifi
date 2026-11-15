/*
 * analog.c: the analog inputs (analog:, the pots, faders, wheels and
 * pressure strips), a control component (README.md here). The main loop
 * (core/src/controller.c gm_poll) calls analog_poll; host feedback calls
 * gm_analog_host. Shares its state through gm_controls.h.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_controls.h"
#include "gm_controller.h"
#include "gm_device.h"
#include "gm_settings.h"
#include "gm_feedback.h"
#include "gm_mcu.h"
#include "gm_vpin.h"
#include "gm_hal.h"

#if GM_N_ANALOGS > 0
static int32_t a_filt[NA];          /* 0..4095 << 2 */
static int16_t a_sent[NA];          /* last 7-bit value, -1 = none yet (analog_init) */
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
            send_event(EV_TOUCH, i, 0);
        }
}
#endif

#if GM_MSG_KINDS
/* What analog.mode sends: K_CC for cc, 0xFF for pitch bend and pressure. */
uint32_t an_kind(int32_t mode)
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
            send_event(EV_TOUCH, i, 1);
        }
    }
#endif
    send_event(EV_ANALOG, i, (uint32_t)pos << 2);
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
    midi3(0xE0 | midi_chan(), (uint32_t)b & 0x7Fu, (uint32_t)b >> 7);
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
int16_t a_raw[NA];
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
void analog_poll(void)
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

void analog_init(void)
{
    for (uint32_t i = 0; i < NA; i++) {
        a_sent[i] = -1;
        bend_scale(i);
    }
    analog_pickup_off();
}

void analog_pickup_off(void)
{
#if GM_ANALOG_PICKUP
    for (uint32_t i = 0; i < NA; i++)
        a_pick[i] = -1;
#endif
}
#endif
