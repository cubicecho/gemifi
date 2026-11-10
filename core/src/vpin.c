/*
 * vpin.c: virtual pins. See gm_vpin.h.
 *
 * Hubs are drivers behind one interface (gm_hub.h): each hub type has a
 * gm_hub_ops_t, in hub_ops[] at the bottom, one section per type above it.
 * Hub control pins (data, clock, latch, select, common, enable) must be MCU
 * pins (hub 0). /E of a mux and /OE of a 595 are driven low (enabled).
 * TM1628 and RGB_SERIAL hubs keep their pins in the hub RAM (hub_ram,
 * GM_TM1628_RAM bytes, each hub's back to back from s->ram), since a chip
 * has more than 32 bits: a TM1628's display RAM, an RGB_SERIAL hub's LEDs a
 * byte per colour (pin = LED * 3 + colour: a pin on is 255, off 0;
 * vpin_rgb sets any colour).
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_vpin.h"
#include "gm_hal.h"
#include "gm_device_config.h"
#include "gm_controller.h"
#include "app_proto.h"
#include "gm_hub.h"

#ifndef GM_TM_FADE
#define GM_TM_FADE 0                /* no tm1628 hub with fade: (gm_device_config.h) */
#endif

#define N_HUBS (GM_N_HUBS > 0 ? GM_N_HUBS : 1)
/* The hub types this device has (bit = GM_HUB_*), from the builder: only
 * their drivers are in hub_ops[], so only they are linked. */
#ifndef GM_HUB_TYPES
#define GM_HUB_TYPES 0xFFFFFFFFu     /* a config generated before it: all */
#endif
#define USES(t) ((GM_HUB_TYPES >> (t)) & 1u)
#ifndef GM_TM1628_RAM
#define GM_TM1628_RAM 0             /* no TM1628 / RGB_SERIAL hub */
#endif

typedef gm_hub_state_t hub_state_t;
static hub_state_t hs[N_HUBS];
static const gm_hub_ops_t *hub_ops_of(uint32_t type);   /* the table, at the bottom */

/* Decoders whose address and enable lines are all on one port with a set/reset
 * register (hal_pin_bsrr): each output is one precomputed store. */
#define DEC_FAST 2u                 /* decoders with a table */
#define DEC_OUT 32u                 /* outputs per table */
static int8_t dec_slot[N_HUBS];     /* table index, -1 = write pin by pin */
static volatile uint32_t *dec_reg[DEC_FAST];
static uint32_t dec_word[DEC_FAST][DEC_OUT];
static uint8_t dec_used;

__attribute__((weak)) int hal_serial_open(uint16_t tx_pin, uint32_t baud, uint32_t flags)
{
    (void)tx_pin;
    (void)baud;
    (void)flags;
    return 0;
}

__attribute__((weak)) int hal_serial_write(uint16_t tx_pin, const uint8_t *b, uint32_t n)
{
    (void)tx_pin;
    (void)b;
    (void)n;
    return 0;
}

__attribute__((weak)) int hal_serial_read(uint16_t tx_pin)
{
    (void)tx_pin;
    return -1;
}

__attribute__((weak)) volatile uint32_t *hal_pin_bsrr(uint16_t pin, uint32_t *bit)
{
    (void)pin;
    *bit = 0;
    return 0;
}

static void dec_fast_init(uint32_t i, const gm_hub_t *h)
{
    dec_slot[i] = -1;
    if (dec_used >= DEC_FAST)
        return;
    volatile uint32_t *reg = 0;
    uint32_t abit[4], ebit[3];
    for (uint32_t k = 0; k < h->n_sel + h->n_en && k < 7u; k++) {
        gm_pin_t p = k < h->n_sel ? h->sel[k] : h->en[k - h->n_sel];
        uint32_t b = 0;
        volatile uint32_t *r = p.hub ? 0 : hal_pin_bsrr(p.num, &b);
        if (!r || (reg && r != reg))
            return;
        reg = r;
        if (k < h->n_sel)
            abit[k] = b;
        else
            ebit[k - h->n_sel] = b;
    }
    if (!reg)
        return;
    uint32_t slot = dec_used++, per = 1u << h->n_sel;
    for (uint32_t n = 0; n < DEC_OUT; n++) {
        uint32_t w = 0, g = n / per, lv = g < 4u ? h->group_lv[g] : 0;
        for (uint32_t k = 0; k < h->n_sel; k++)
            w |= ((n >> k) & 1u) ? 1u << abit[k] : 1u << (abit[k] + 16u);
        for (uint32_t k = 0; k < h->n_en; k++)
            w |= ((lv >> k) & 1u) ? 1u << ebit[k] : 1u << (ebit[k] + 16u);
        dec_word[slot][n] = w;
    }
    dec_reg[slot] = reg;
    dec_slot[i] = (int8_t)slot;
}

/* Switch matrices whose columns are adjacent bits of one input register, in
 * order (the Code 61's PB8-PB15): one load per row instead of a read per pin.
 * sm_reg 0 = read pin by pin. */
static const volatile uint32_t *sm_reg[N_HUBS];
static uint8_t sm_shift[N_HUBS];
static uint32_t sm_inv[N_HUBS];     /* columns with GM_PIN_INVERT */

static void sm_fast_init(uint32_t i, const gm_hub_t *h)
{
    sm_reg[i] = 0;
    const volatile uint32_t *reg = 0;
    uint32_t inv = 0;
    for (uint32_t c = 0; c < h->n_cols; c++) {
        uint32_t mask = 0;
        const volatile uint32_t *r = h->cols[c].hub ? 0 : hal_pin_in_reg(h->cols[c].num, &mask);
        if (!r || !mask || (mask & (mask - 1u)))
            return;
        uint32_t bit = (uint32_t)__builtin_ctz(mask);
        if (c == 0) {
            reg = r;
            sm_shift[i] = (uint8_t)bit;
        } else if (r != reg || bit != sm_shift[i] + c) {
            return;
        }
        if (h->cols[c].flags & GM_PIN_INVERT)
            inv |= 1u << c;
    }
    if (h->n_cols == 0 || sm_shift[i] + h->n_cols > 32u)
        return;
    sm_inv[i] = inv;
    sm_reg[i] = reg;
}
#if GM_TM1628_RAM > 0
static uint8_t hub_ram[GM_TM1628_RAM];
static uint8_t ram_sent[GM_TM1628_RAM];   /* what the chip / LED board has (its bytes of hub_ram) */
static uint32_t ram_next;                 /* vpin_init: the next free byte */

/* n bytes of the hub RAM for the hub being initialised (as many as are left). */
static uint16_t ram_alloc(uint32_t n)
{
    uint32_t at = ram_next;
    ram_next = at + n < GM_TM1628_RAM ? at + n : GM_TM1628_RAM;
    return (uint16_t)at;
}
#endif

static const gm_hub_t *hub_of(gm_pin_t p)
{
    if (p.hub == 0 || p.hub == GM_HUB_NONE || p.hub > gm_device.n_hubs)
        return 0;
    return &gm_device.hubs[p.hub - 1];
}

int vpin_is_none(gm_pin_t p)
{
    return p.hub == GM_HUB_NONE;
}

int vpin_on_i2c(gm_pin_t p)
{
    return (vpin_pin_caps(p) & VPIN_BUS_IO) != 0;
}

/* What a hub of this type needs from the controller (gm_vpin.h VPIN_*). */
static uint32_t type_caps(uint32_t type)
{
    const gm_hub_ops_t *o = hub_ops_of(type);
    uint32_t c = 0;
    if (o->refresh || o->flush)
        c |= (o->flags & GM_HUBF_BUS) ? VPIN_BUS_IO : VPIN_SCAN_IO;
    if (o->send || o->poll)
        c |= VPIN_MAIN;
    if (o->flags & GM_HUBF_MATRIX)
        c |= VPIN_MATRIX;
    if (o->rgb)
        c |= VPIN_RGB;
    return c;
}

uint32_t vpin_caps(void)
{
    uint32_t c = 0;
    for (uint32_t i = 0; i < gm_device.n_hubs; i++)
        c |= type_caps(gm_device.hubs[i].type);
    return c;
}

uint32_t vpin_pin_caps(gm_pin_t p)
{
    const gm_hub_t *h = hub_of(p);
    return h ? type_caps(h->type) : 0;
}

static void mcu_out(gm_pin_t p, int level)
{
    if (p.hub == 0) {
        hal_pin_write(p.num, level);    /* first too: no glitch where it can */
        hal_pin_mode(p.num, (p.flags & GM_PIN_OPEN_DRAIN) ? HAL_OUT_OD : HAL_OUT);
        hal_pin_write(p.num, level);
    }
}

/* ---- the generic part: each pin to its hub's driver -------------------- */

/* Hardware scan (vpin_scan_plan): which plan slot each switch-matrix row
 * reads from. */
#define FED_MAX 32u
static uint8_t fed_hub[FED_MAX], fed_row[FED_MAX], fed_slot[FED_MAX];
static uint8_t n_fed, fed_on;
static void adc_off(void);          /* the hardware analog scan, below */

/* Hubs refreshed / flushed from the scan tick, in hub order: the tick goes
 * through these lists, not every hub. */
static uint8_t scan_in[N_HUBS], scan_out[N_HUBS];
static uint8_t n_scan_in, n_scan_out;

void vpin_init(void)
{
#if GM_TM1628_RAM > 0
    ram_next = 0;
#endif
    n_fed = 0;
    fed_on = 0;
    n_scan_in = n_scan_out = 0;
    adc_off();
    for (uint32_t i = 0; i < gm_device.n_hubs; i++) {
        const gm_hub_t *h = &gm_device.hubs[i];
        const gm_hub_ops_t *o = hub_ops_of(h->type);
        hub_state_t *s = &hs[i];
        *s = (hub_state_t){ 0 };
        s->never = 1;
        s->channel = -1;
        s->flags = (uint8_t)(o->flags | (o->read ? GM_HUBF_READ : 0u) | (o->write ? GM_HUBF_WRITE : 0u));
        dec_slot[i] = -1;
        if (!(o->flags & GM_HUBF_BUS)) {
            if (o->refresh)
                scan_in[n_scan_in++] = (uint8_t)i;
            if (o->flush)
                scan_out[n_scan_out++] = (uint8_t)i;
        }
        if (o->init)
            o->init(h, s);
    }
}

void vpin_input(gm_pin_t p)
{
    if (p.hub == 0) {
        hal_pin_mode(p.num, (p.flags & GM_PIN_PULLUP) ? HAL_IN_PULLUP :
                            (p.flags & GM_PIN_PULLDOWN) ? HAL_IN_PULLDOWN : HAL_IN);
        return;
    }
    const gm_hub_t *h = hub_of(p);
    const gm_hub_ops_t *o = h ? hub_ops_of(h->type) : 0;
    if (o && o->input)
        o->input(h, &hs[p.hub - 1], p);
}

void vpin_output(gm_pin_t p)
{
    if (p.hub == 0) {
        hal_pin_mode(p.num, HAL_OUT);
        hal_pin_write(p.num, (p.flags & GM_PIN_INVERT) ? 1 : 0);
        return;
    }
    const gm_hub_t *h = hub_of(p);
    if (!h)
        return;
    const gm_hub_ops_t *o = hub_ops_of(h->type);
    if (o->output)
        o->output(h, &hs[p.hub - 1], p);
    vpin_write(p, 0);
}

static void mux_select(const gm_hub_t *h, hub_state_t *s, uint32_t ch);
static void decoder_select(const gm_hub_t *h, hub_state_t *s, uint32_t n);

__attribute__((weak)) const volatile uint32_t *hal_pin_in_reg(uint16_t pin, uint32_t *mask)
{
    (void)pin;
    *mask = 0;
    return 0;
}

__attribute__((weak)) volatile uint32_t *hal_pin_out_reg(uint16_t pin)
{
    (void)pin;
    return 0;
}

/* The scan reads mux channels and cached inputs: those are done here, not
 * through the table (a mux has no read op). */
int vpin_read(gm_pin_t p)
{
    int inv = (p.flags & GM_PIN_INVERT) ? 1 : 0;
    if (p.hub == 0)
        return hal_pin_read(p.num) ^ inv;
    const gm_hub_t *h = hub_of(p);
    if (!h)
        return 0;
    hub_state_t *s = &hs[p.hub - 1];
    if (h->type == GM_HUB_MUX) {
        if (h->common.hub != 0)
            return 0;
        mux_select(h, s, p.num);
        return hal_pin_read(h->common.num) ^ inv;
    }
    if (s->flags & (GM_HUBF_NO_READ | GM_HUBF_READ)) {
        if (s->flags & GM_HUBF_NO_READ)
            return 0;                   /* outputs only */
        return hub_ops_of(h->type)->read(h, s, p.num) ^ inv;
    }
    return (int)((s->in >> p.num) & 1u) ^ inv;
}

/* The scan drives decoder rows and cached outputs: those are done here, not
 * through the table (a decoder has no write op). */
void vpin_write(gm_pin_t p, int active)
{
    int level = (active ? 1 : 0) ^ ((p.flags & GM_PIN_INVERT) ? 1 : 0);
    if (p.hub == 0) {
        hal_pin_write(p.num, level);
        return;
    }
    const gm_hub_t *h = hub_of(p);
    if (!h)
        return;
    hub_state_t *s = &hs[p.hub - 1];
    if (h->type == GM_HUB_DECODER) {
        if (active)
            decoder_select(h, s, p.num);
        return;
    }
    if (s->flags & GM_HUBF_WRITE) {
        hub_ops_of(h->type)->write(h, s, p.num, level);
        return;
    }
    if (level)
        s->out |= 1u << p.num;
    else
        s->out &= ~(1u << p.num);
}

int vpin_rgb(gm_pin_t p, uint32_t rgb)
{
    const gm_hub_t *h = p.hub ? hub_of(p) : 0;
    const gm_hub_ops_t *o = h ? hub_ops_of(h->type) : 0;
    return o && o->rgb ? o->rgb(h, &hs[p.hub - 1], p.num, rgb) : 0;
}

/* Software reading of p (vpin_analog: the hardware analog scan is first). */
static int32_t analog_sw(gm_pin_t p)
{
    if (p.hub == 0)
        return hal_adc_read(p.num);
    const gm_hub_t *h = hub_of(p);
    const gm_hub_ops_t *o = h ? hub_ops_of(h->type) : 0;
    return o && o->analog ? o->analog(h, &hs[p.hub - 1], p.num) : -1;
}

void vpin_refresh(void)
{
    for (uint32_t k = 0; GM_N_HUBS > 0 && k < n_scan_in; k++) {
        uint32_t i = scan_in[k];
        const gm_hub_t *h = &gm_device.hubs[i];
        hub_ops_of(h->type)->refresh(h, &hs[i]);
    }
}

void vpin_flush(void)
{
    for (uint32_t k = 0; GM_N_HUBS > 0 && k < n_scan_out; k++) {
        uint32_t i = scan_out[k];
        const gm_hub_t *h = &gm_device.hubs[i];
        hub_ops_of(h->type)->flush(h, &hs[i]);
    }
}

/* Every I2C_MS from the main loop. */
void vpin_refresh_i2c(void)
{
    for (uint32_t i = 0; i < gm_device.n_hubs; i++) {
        const gm_hub_t *h = &gm_device.hubs[i];
        const gm_hub_ops_t *o = hub_ops_of(h->type);
        if ((o->flags & GM_HUBF_BUS) && o->refresh)
            o->refresh(h, &hs[i]);
    }
}

void vpin_flush_i2c(void)
{
    for (uint32_t i = 0; i < gm_device.n_hubs; i++) {
        const gm_hub_t *h = &gm_device.hubs[i];
        const gm_hub_ops_t *o = hub_ops_of(h->type);
        if ((o->flags & GM_HUBF_BUS) && o->flush)
            o->flush(h, &hs[i]);
    }
}

/* Each hub's send in turn, sharing budget; returns what's left. */
static uint32_t send_all(uint32_t budget)
{
    for (uint32_t i = 0; i < gm_device.n_hubs && budget; i++) {
        const gm_hub_t *h = &gm_device.hubs[i];
        const gm_hub_ops_t *o = hub_ops_of(h->type);
        if (o->send)
            budget = o->send(h, &hs[i], budget);
    }
    return budget;
}

void vpin_send_all(void)
{
    (void)send_all(0xFFFFFFFFu);
}

/* 4 bus bytes: ~50 us at most per main-loop pass on the STM32 (a byte is
 * ~11 us, a command ~3 us more), and a single LED change (3 bytes in two
 * commands, ~40 us) always goes out at once. A run goes on 2 bytes a pass:
 * the display's 12 bytes take 6 passes, a whole 14-byte chip 7 (a few ms
 * at the main loop's rate, within one 10 ms LED round). */
#ifndef TM_POLL_BUDGET
#define TM_POLL_BUDGET 4u
#endif

void vpin_poll(uint32_t now_ms, int busy)
{
    if (!busy)
        (void)send_all(TM_POLL_BUDGET);
    for (uint32_t i = 0; i < gm_device.n_hubs; i++) {
        const gm_hub_t *h = &gm_device.hubs[i];
        const gm_hub_ops_t *o = hub_ops_of(h->type);
        if (o->poll)
            o->poll(h, &hs[i], now_ms);
    }
}

void vpin_flush_tm1628(void)
{
    vpin_send_all();
}

void vpin_poll_tm1628(void)
{
    (void)send_all(TM_POLL_BUDGET);
}

/* ---- MCU pins as hub control lines ------------------------------------ */

static void mcu_in(gm_pin_t p)
{
    if (p.hub == 0)
        hal_pin_mode(p.num, (p.flags & GM_PIN_PULLUP) ? HAL_IN_PULLUP :
                            (p.flags & GM_PIN_PULLDOWN) ? HAL_IN_PULLDOWN : HAL_IN);
}

/* ---- 74HC595 / 74HC165 shift registers (scan tick) --------------------- */

static void pulse(uint16_t pin)
{
    hal_pin_write(pin, 1);
    hal_pin_write(pin, 0);
}

static void hc595_init(const gm_hub_t *h, hub_state_t *s)
{
    (void)s;
    mcu_out(h->data, 0);
    mcu_out(h->clock, 0);
    mcu_out(h->latch, 0);
    mcu_out(h->enable, 0);
}

static void hc595_flush(const gm_hub_t *h, hub_state_t *s)
{
    if (s->out == s->written && !s->never)
        return;
    /* The first bit shifted ends up in the last chip: send the highest
     * pin first. */
    uint32_t bits = 8u * (h->chain ? h->chain : 1u);
    if (bits > 32)
        bits = 32;
    uint32_t v = s->out;
    for (uint32_t b = bits; b-- > 0;) {
        hal_pin_write(h->data.num, (v >> b) & 1u);
        pulse(h->clock.num);
    }
    pulse(h->latch.num);
    s->written = v;
    s->never = 0;
}

static void hc165_init(const gm_hub_t *h, hub_state_t *s)
{
    (void)s;
    mcu_out(h->clock, 0);
    mcu_out(h->latch, 1);
    if (h->data.hub == 0)
        hal_pin_mode(h->data.num, HAL_IN);
}

static void hc165_refresh(const gm_hub_t *h, hub_state_t *s)
{
    /* Pin numbers as ESPHome: 0-7 on the chip wired to the MCU, 0 = its A.
     * SH/LD low loads the parallel inputs; then QH = first chip's H
     * (pin 7), and each clock shifts the next bit in. */
    hal_pin_write(h->latch.num, 0);
    hal_delay_us(1);
    hal_pin_write(h->latch.num, 1);
    uint32_t bits = 8u * (h->chain ? h->chain : 1u), v = 0;
    if (bits > 32)
        bits = 32;
    for (uint32_t b = 0; b < bits; b++) {
        uint32_t pin = (b / 8u) * 8u + 7u - (b % 8u);
        if (hal_pin_read(h->data.num))
            v |= 1u << pin;
        pulse(h->clock.num);
    }
    s->in = v;
}

/* ---- XY pads (4-wire resistive) ----------------------------------------
 * One plate is driven at a time, + pin high and - pin low; the other plate
 * floats and takes the voltage where it's pressed, read on its sense pin
 * (gm_hub_t: data for X, clock for Y). Untouched, the floating plate reads
 * outside touch_min..touch_max (stock Code 61: 592 - 22 .. 3408 + 22). The
 * other plate is let go before this one is driven, so a touch never joins
 * two driven pins. */
static void xy_drive(const gm_hub_t *h, uint32_t axis)
{
    const gm_pin_t *drv = &h->sel[axis ? 2 : 0], *oth = &h->sel[axis ? 0 : 2];
    gm_pin_t sense = axis ? h->clock : h->data;
    for (uint32_t k = 0; k < 2; k++)
        if (oth[k].hub == 0)
            hal_pin_mode(oth[k].num, oth[k].num == sense.num ? HAL_ANALOG : HAL_IN);
    mcu_out(drv[0], 1);
    mcu_out(drv[1], 0);
}

static void xy_init(const gm_hub_t *h, hub_state_t *s)
{
    s->u.xy.raw[0] = s->u.xy.raw[1] = 0xFFFFu;
    xy_drive(h, 0);
    s->channel = 0;
    s->u.xy.t = hal_millis();
}

/* A touch counts after XY_SETTLE readings in a row inside the window
 * (each axis twice, ~4 ms): the first as a finger lands can be anywhere. */
#define XY_SETTLE 4u

/* Once per ms: read the axis driven since the last switch (a whole ms to
 * settle), then drive the other. So each axis is new every 2 ms. -1 while
 * untouched (the input holds its last value). */
static int32_t xy_analog(const gm_hub_t *h, hub_state_t *s, uint32_t ch)
{
    uint32_t now = hal_millis();
    if (now != s->u.xy.t && s->channel >= 0) {
        uint32_t a = (uint32_t)s->channel;
        gm_pin_t sense = a ? h->clock : h->data;
        int32_t v = sense.hub == 0 ? hal_adc_read(sense.num) : -1;
        s->u.xy.raw[a] = v < 0 ? 0xFFFFu : (uint16_t)v;
        int t = 1;
        for (uint32_t k = 0; k < 2; k++)
            t = t && s->u.xy.raw[k] >= h->touch_min && s->u.xy.raw[k] <= h->touch_max;
        if (!t)
            s->u.xy.touch = 0;
        else if (s->u.xy.touch < XY_SETTLE && ++s->u.xy.touch == XY_SETTLE)
            s->u.xy.fresh = 3u;
        s->channel = (int8_t)(a ^ 1u);
        xy_drive(h, a ^ 1u);
        s->u.xy.t = now;
    }
    if (ch > 1u || s->u.xy.touch < XY_SETTLE)
        return -1;
    return s->u.xy.raw[ch];
}

int vpin_analog_jump(gm_pin_t p)
{
    const gm_hub_t *h = hub_of(p);
    if (!h || h->type != GM_HUB_XY || p.num > 1u)
        return 0;
    hub_state_t *s = &hs[p.hub - 1];
    uint8_t bit = (uint8_t)(1u << p.num);
    if (!(s->u.xy.fresh & bit))
        return 0;
    s->u.xy.fresh &= (uint8_t)~bit;
    return 1;
}

/* ---- 74HC4051 / 4067 multiplexers --------------------------------------
 * Muxes may share select lines (allow_other_uses): switching one switches
 * the others. One with the same lines is now on the same channel; one that
 * shares only some must switch again. Which muxes share select lines with
 * which is worked out once (mux_init): bit j of mux_same[i] = hub j has
 * exactly hub i's lines (so it is now on the same channel), of mux_some[i] =
 * it shares only some (so it must switch again). Masks cover hubs 0-31; a
 * device with more falls back to the loop. Read digitally by vpin_read
 * itself (the scan's path), as analog by the analog op or the hardware
 * analog scan. */
static uint32_t mux_same[N_HUBS], mux_some[N_HUBS];
#define MUX_MASKS (gm_device.n_hubs <= 32u)

static void mux_pair(const gm_hub_t *h, const gm_hub_t *o, int *any, int *same)
{
    uint32_t n = 0;
    *any = 0;
    for (uint32_t k = 0; k < o->n_sel && k < 4; k++)
        for (uint32_t j = 0; j < h->n_sel && j < 4; j++)
            if (o->sel[k].hub == 0 && h->sel[j].hub == 0 && o->sel[k].num == h->sel[j].num) {
                *any = 1;
                n += j == k;
            }
    *same = n == o->n_sel && o->n_sel == h->n_sel;
}

static void mux_init(const gm_hub_t *h, hub_state_t *s)
{
    (void)s;
    for (uint32_t k = 0; k < h->n_sel && k < 4; k++)
        mcu_out(h->sel[k], 0);
    mcu_out(h->enable, 0);
    mcu_in(h->common);
    uint32_t i = (uint32_t)(h - gm_device.hubs);
    mux_same[i] = mux_some[i] = 0;
    for (uint32_t j = 0; MUX_MASKS && j < gm_device.n_hubs; j++) {
        const gm_hub_t *o = &gm_device.hubs[j];
        int any, same;
        if (j == i || o->type != GM_HUB_MUX)
            continue;
        mux_pair(h, o, &any, &same);
        if (any)
            *(same ? &mux_same[i] : &mux_some[i]) |= 1u << j;
    }
}

static void mux_shared(const gm_hub_t *h, uint32_t ch)
{
    uint32_t i = (uint32_t)(h - gm_device.hubs);
    if (MUX_MASKS) {
        for (uint32_t m = mux_same[i]; m; m &= m - 1u)
            hs[__builtin_ctz(m)].channel = (int8_t)ch;
        for (uint32_t m = mux_some[i]; m; m &= m - 1u)
            hs[__builtin_ctz(m)].channel = -1;
        return;
    }
    for (uint32_t j = 0; j < gm_device.n_hubs; j++) {
        const gm_hub_t *o = &gm_device.hubs[j];
        int any, same;
        if (o == h || o->type != GM_HUB_MUX)
            continue;
        mux_pair(h, o, &any, &same);
        if (any)
            hs[j].channel = (int8_t)(same ? (int32_t)ch : -1);
    }
}

static void mux_select(const gm_hub_t *h, hub_state_t *s, uint32_t ch)
{
    if (s->channel == (int8_t)ch)
        return;
    for (uint32_t k = 0; k < h->n_sel && k < 4; k++)
        if (h->sel[k].hub == 0)
            hal_pin_write(h->sel[k].num, (ch >> k) & 1u);
    s->channel = (int8_t)ch;
    mux_shared(h, ch);
    hal_delay_us(h->settle_us ? h->settle_us : 2);
}

static int32_t mux_analog(const gm_hub_t *h, hub_state_t *s, uint32_t ch)
{
    if (h->common.hub != 0)
        return -1;
    mux_select(h, s, ch);
    return hal_adc_read(h->common.num);
}

/* ---- decoders (74HC138 style) ------------------------------------------
 * Written by vpin_write itself (the scan's path): "on" selects the output,
 * "off" does nothing (the next row's select replaces it). */

static void decoder_init(const gm_hub_t *h, hub_state_t *s)
{
    for (uint32_t k = 0; k < h->n_sel && k < 4; k++)
        mcu_out(h->sel[k], 0);
    for (uint32_t k = 0; k < h->n_en && k < 3; k++)
        mcu_out(h->en[k], 0);
    dec_fast_init((uint32_t)(s - hs), h);
}

/* Decoder: address lines = n's low bits, enable lines = its group's levels.
 * Written only when the selection changes. */
static void decoder_select(const gm_hub_t *h, hub_state_t *s, uint32_t n)
{
    /* One store: always written, so a key scan that selects rows itself
     * (vpin_select_word) never leaves this out of date. */
    int8_t slot = dec_slot[s - hs];
    if (slot >= 0 && n < DEC_OUT) {
        *dec_reg[slot] = dec_word[slot][n];
        return;
    }
    if (s->channel == (int8_t)n)
        return;
    uint32_t g = n >> h->n_sel;
    for (uint32_t k = 0; k < h->n_sel && k < 4; k++)
        if (h->sel[k].hub == 0)
            hal_pin_write(h->sel[k].num, (n >> k) & 1u);
    uint32_t lv = g < 4 ? h->group_lv[g] : 0;
    for (uint32_t k = 0; k < h->n_en && k < 3; k++)
        if (h->en[k].hub == 0)
            hal_pin_write(h->en[k].num, (lv >> k) & 1u);
    s->channel = (int8_t)n;
}

/* ---- hardware analog scan (gm_controller.h gm_adc_plan_t) -------------- */

#ifndef GM_HW_ADC
#define GM_HW_ADC 0                 /* a config generated before hardware_scan: */
#endif
#if GM_HW_ADC
/* Sized for this device's plan (the builder's GM_HW_ADC_PINS / _STEPS),
 * else for the largest. */
#ifdef GM_HW_ADC_PINS
#define ADC_PINS GM_HW_ADC_PINS
#define ADC_STEPS GM_HW_ADC_STEPS
#else
#define ADC_PINS GM_ADC_MAX_PINS
#define ADC_STEPS GM_ADC_MAX_STEPS
#endif
#define ADC_CELLS (ADC_STEPS * ADC_PINS)
static gm_adc_plan_t aplan;
static uint8_t adc_on;
static volatile uint8_t adc_swept;     /* a whole sweep is in adc_last[] since the start */
static uint16_t adc_last[ADC_CELLS];
/* Per cell, the highest and lowest reading of the sweeps since the pads'
 * last look, in two generations: the interrupt fills adc_gen while the main
 * loop reads the other (vpin_analog_peaks flips them). The interrupt can't
 * be interrupted by the main loop, so a flip never lands mid-sweep. */
static uint16_t adc_hi[2][ADC_CELLS], adc_lo[2][ADC_CELLS];
static volatile uint32_t adc_n[2];
static volatile uint8_t adc_gen;
static uint8_t adc_read_gen;

static int adc_add_pin(uint16_t pin)
{
    for (uint32_t k = 0; k < aplan.n_pins; k++)
        if (aplan.pin[k] == pin)
            return (int)k;
    if (aplan.n_pins >= ADC_PINS)
        return -1;
    aplan.pin[aplan.n_pins] = pin;
    return (int)aplan.n_pins++;
}

static int adc_pin_index(uint16_t pin)
{
    for (uint32_t k = 0; k < aplan.n_pins; k++)
        if (aplan.pin[k] == pin)
            return (int)k;
    return -1;
}

/* One analog source into the plan: an ADC pin, or a mux channel whose select
 * lines are the plan's (the first mux sets them). 0 if it can't be. */
static int adc_plan_source(gm_pin_t p, const gm_hub_t **group)
{
    if (p.hub == 0)
        return adc_add_pin(p.num) >= 0;
    const gm_hub_t *h = hub_of(p);
    if (!h || h->type != GM_HUB_MUX || h->common.hub != 0 || h->n_sel == 0 || h->n_sel > 4)
        return 0;
    if (!*group) {
        uint32_t steps = 1u << h->n_sel;
        if (steps > ADC_STEPS)
            return 0;
        volatile uint32_t *reg = 0;
        uint32_t bit[4];
        for (uint32_t k = 0; k < h->n_sel; k++) {
            volatile uint32_t *r = h->sel[k].hub ? 0 : hal_pin_bsrr(h->sel[k].num, &bit[k]);
            if (!r || (reg && r != reg))
                return 0;
            reg = r;
        }
        for (uint32_t c = 0; c < steps; c++) {
            uint32_t w = 0;
            for (uint32_t k = 0; k < h->n_sel; k++)
                w |= ((c >> k) & 1u) ? 1u << bit[k] : 1u << (bit[k] + 16u);
            aplan.step_word[c] = w;
        }
        aplan.sel_reg = reg;
        aplan.n_steps = steps;
        *group = h;
    } else if (h != *group) {
        const gm_hub_t *g = *group;
        if (h->n_sel != g->n_sel)
            return 0;
        for (uint32_t k = 0; k < h->n_sel; k++)
            if (h->sel[k].hub != 0 || h->sel[k].num != g->sel[k].num)
                return 0;
    }
    uint32_t ns = (h->settle_us ? h->settle_us : 2u) * 1000u;   /* as mux_select waits */
    if (ns > aplan.settle_ns)
        aplan.settle_ns = ns;
    return adc_add_pin(h->common.num) >= 0;
}

static int on_mux(gm_pin_t p)
{
    const gm_hub_t *h = p.hub ? hub_of(p) : 0;
    return h && h->type == GM_HUB_MUX;
}
#endif

static void adc_off(void)
{
#if GM_HW_ADC
    adc_on = 0;
#endif
}

int gm_adc_hw(void)
{
#if GM_HW_ADC
    return adc_on;
#else
    return 0;
#endif
}

const gm_adc_plan_t *gm_adc_plan(void)
{
#if GM_HW_ADC
    const gm_hub_t *group = 0;
    aplan.sel_reg = 0;
    aplan.n_steps = 1;
    aplan.step_word[0] = 0;
    aplan.n_pins = 0;
    aplan.settle_ns = 0;
    int ok = 1;
    for (uint32_t i = 0; ok && i < gm_device.n_analogs; i++) {
        const gm_hub_t *h = hub_of(gm_device.analogs[i].pin);
        if (h && h->type == GM_HUB_XY)
            continue;               /* read in software (ADC2 on stm32f1) */
        ok = adc_plan_source(gm_device.analogs[i].pin, &group);
    }
#if GM_N_PADS > 0
    for (uint32_t i = 0; ok && i < GM_N_PADS; i++)
        ok = adc_plan_source(gm_pads[i].pin, &group);
#endif
    /* A button or encoder on a mux reads it digitally, switching the select
     * lines the hardware is stepping: not with this. */
    for (uint32_t i = 0; ok && i < gm_device.n_buttons; i++)
        ok = !on_mux(gm_device.buttons[i].pin);
    for (uint32_t i = 0; ok && i < gm_device.n_encoders; i++)
        ok = !on_mux(gm_device.encoders[i].a) && !on_mux(gm_device.encoders[i].b);
    if (!ok || !aplan.n_pins) {
        app_log(LOG_WARN, "hardware analog scan: inputs on muxes with different select lines, a mux ",
                "read digitally, or too many ADC pins; reading in software");
        return 0;
    }
    return &aplan;
#else
    return 0;
#endif
}

void gm_adc_hw_started(void)
{
#if GM_HW_ADC
    adc_n[0] = adc_n[1] = 0;
    adc_swept = 0;
    adc_gen = 0;
    adc_read_gen = 1;
    adc_on = 1;
#endif
}

void gm_adc_sweep(const uint16_t *v)
{
#if GM_HW_ADC
    uint32_t g = adc_gen, n = aplan.n_steps * aplan.n_pins, first = adc_n[g] == 0;
    uint16_t *hi = adc_hi[g], *lo = adc_lo[g];
    for (uint32_t i = 0; i < n; i++) {
        uint16_t x = (uint16_t)(v[i] & 0xFFFu);
        adc_last[i] = x;
        if (first || x > hi[i])
            hi[i] = x;
        if (first || x < lo[i])
            lo[i] = x;
    }
    adc_n[g]++;
    adc_swept = 1;
#else
    (void)v;
#endif
}

void vpin_analog_peaks(void)
{
#if GM_HW_ADC
    if (!adc_on)
        return;
    uint8_t g = adc_gen;
    adc_n[g ^ 1u] = 0;              /* the one read last time: refilled from scratch */
    adc_gen = (uint8_t)(g ^ 1u);
    adc_read_gen = g;
#endif
}

/* Cell lookup for p: its pin index k and step (-1 = an ADC pin: every step). */
#if GM_HW_ADC
static int adc_cell(gm_pin_t p, int *k, int *step)
{
    if (p.hub == 0) {
        *k = adc_pin_index(p.num);
        *step = -1;
    } else {
        const gm_hub_t *h = hub_of(p);
        if (!h || h->type != GM_HUB_MUX || p.num >= aplan.n_steps)
            return 0;
        *k = adc_pin_index(h->common.num);
        *step = (int)p.num;
    }
    return *k >= 0;
}
#endif

int32_t vpin_analog_peak(gm_pin_t p)
{
#if GM_HW_ADC
    int k, step;
    uint32_t g = adc_read_gen;
    if (adc_on && adc_n[g] && adc_cell(p, &k, &step)) {
        int inv = (p.flags & GM_PIN_INVERT) != 0;
        int32_t best = -1;
        for (uint32_t s = step < 0 ? 0u : (uint32_t)step; s < aplan.n_steps; s++) {
            uint32_t c = s * aplan.n_pins + (uint32_t)k;
            int32_t v = inv ? 4095 - adc_lo[g][c] : adc_hi[g][c];
            if (v > best)
                best = v;
            if (step >= 0)
                break;
        }
        return best;
    }
#endif
    return vpin_analog(p);
}

int32_t vpin_analog(gm_pin_t p)
{
    int32_t v;
#if GM_HW_ADC
    int k, step;
    if (adc_on && adc_cell(p, &k, &step)) {
        if (!adc_swept)
            return -1;                  /* no sweep yet: adc_last[] is still zeros, not a reading */
        if (step >= 0) {
            v = adc_last[(uint32_t)step * aplan.n_pins + (uint32_t)k];
        } else {
            uint32_t sum = 0;           /* an ADC pin: the sweep's mean (n_steps readings) */
            for (uint32_t s = 0; s < aplan.n_steps; s++)
                sum += adc_last[s * aplan.n_pins + (uint32_t)k];
            v = (int32_t)(sum / aplan.n_steps);
        }
        return (p.flags & GM_PIN_INVERT) ? 4095 - v : v;
    }
#endif
    v = analog_sw(p);
    if (v >= 0 && (p.flags & GM_PIN_INVERT))
        v = 4095 - v;
    return v;
}

/* ---- switch matrices ---------------------------------------------------
 * Scanned from the scan tick by vpin_scan_matrices (or fed by the hardware
 * scan, vpin_scan_feed), not through the table: the scan's own path. */

static void swmatrix_init(const gm_hub_t *h, hub_state_t *s)
{
    for (uint32_t c = 0; c < h->n_cols; c++)
        vpin_input(h->cols[c]);
    for (uint32_t r = 0; r < h->n_rows; r++)
        vpin_output(h->rows[r]);
    sm_fast_init((uint32_t)(s - hs), h);
}

int vpin_select_word(gm_pin_t p, volatile uint32_t **reg, uint32_t *word)
{
    const gm_hub_t *h = hub_of(p);
    if (!h || h->type != GM_HUB_DECODER)
        return 0;
    int8_t slot = dec_slot[p.hub - 1];
    if (slot < 0 || p.num >= DEC_OUT)
        return 0;
    *reg = dec_reg[slot];
    *word = dec_word[slot][p.num];
    return 1;
}

int vpin_is_decoder(gm_pin_t p)
{
    const gm_hub_t *h = hub_of(p);
    return h && h->type == GM_HUB_DECODER;
}

const volatile uint32_t *vpin_in_reg(gm_pin_t p, uint32_t *mask)
{
    if (p.hub == 0)
        return hal_pin_in_reg(p.num, mask);
    const gm_hub_t *h = hub_of(p);
    if (!h || !(hub_ops_of(h->type)->flags & GM_HUBF_IN_REG))
        return 0;
    *mask = 1u << p.num;
    return &hs[p.hub - 1].in;
}

int vpin_has_matrices(void)
{
    for (uint32_t i = 0; i < gm_device.n_hubs; i++)
        if (gm_device.hubs[i].type == GM_HUB_SWMATRIX)
            return 1;
    return 0;
}

/* One pass over every switch matrix: drive a row, wait, read the columns.
 * Bit n of the cache = row n / n_cols, column n % n_cols, 1 = closed. */
void vpin_scan_matrices(void)
{
    for (uint32_t i = 0; i < gm_device.n_hubs; i++) {
        const gm_hub_t *h = &gm_device.hubs[i];
        if (h->type != GM_HUB_SWMATRIX)
            continue;
        if (fed_on)
            break;                      /* every switch matrix is fed by frames */
        hub_state_t *s = &hs[i];
        uint32_t v = s->in;
        uint32_t all = h->n_cols >= 32u ? 0xFFFFFFFFu : (1u << h->n_cols) - 1u;
        /* rows_per_scan rows per call, in turn (the cursor lives in .channel,
         * unused by switch matrices); the other rows keep their last reading. */
        uint32_t n = h->rows_per_scan && h->rows_per_scan < h->n_rows ? h->rows_per_scan : h->n_rows;
        uint32_t r = (s->channel >= 0 && (uint32_t)s->channel < h->n_rows) ? (uint32_t)s->channel : 0;
        for (uint32_t k = 0; k < n; k++, r = r + 1u < h->n_rows ? r + 1u : 0) {
            uint32_t bit = r * h->n_cols;
            if (bit >= 32u)
                continue;
            vpin_write(h->rows[r], 1);
            hal_delay_us(h->settle_us ? h->settle_us : 1);
            uint32_t row = 0;
            if (sm_reg[i]) {
                row = ((*sm_reg[i] >> sm_shift[i]) & all) ^ sm_inv[i];
            } else {
                for (uint32_t c = 0; c < h->n_cols && bit + c < 32u; c++)
                    if (vpin_read(h->cols[c]))
                        row |= 1u << c;
            }
            v = (v & ~(all << bit)) | (row << bit);
            if (!vpin_is_decoder(h->rows[r]))
                vpin_write(h->rows[r], 0);
        }
        s->channel = (int8_t)r;
        s->in = v;
        s->u.sm.seen_hi |= v;
        s->u.sm.seen_lo |= ~v;
    }
}

int vpin_scan_plan(volatile uint32_t **sel_reg, uint32_t *sel_word, uint32_t *n_rows,
                   uint32_t max_rows, const volatile uint32_t **in_reg, uint32_t *in_mask)
{
    volatile uint32_t *sreg = *sel_reg;
    const volatile uint32_t *ireg = *in_reg;
    uint32_t n = *n_rows, mask = *in_mask, nf = 0;
    for (uint32_t i = 0; i < gm_device.n_hubs; i++) {
        const gm_hub_t *h = &gm_device.hubs[i];
        if (h->type != GM_HUB_SWMATRIX)
            continue;
        if (!sm_reg[i] || (ireg && sm_reg[i] != ireg))
            return 0;                   /* columns not one load of the plan's register */
        ireg = sm_reg[i];
        uint32_t all = h->n_cols >= 32u ? 0xFFFFFFFFu : (1u << h->n_cols) - 1u;
        mask |= all << sm_shift[i];
        for (uint32_t r = 0; r < h->n_rows; r++) {
            if (r * h->n_cols >= 32u)
                break;                  /* beyond the 32-bit cache, as vpin_scan_matrices */
            volatile uint32_t *reg = 0;
            uint32_t word = 0, k;
            if (!vpin_select_word(h->rows[r], &reg, &word) || (sreg && reg != sreg))
                return 0;
            sreg = reg;
            for (k = 0; k < n && sel_word[k] != word; k++) {
            }
            if (k == n) {
                if (n >= max_rows)
                    return 0;
                sel_word[n++] = word;
            }
            if (nf >= FED_MAX)
                return 0;
            fed_hub[nf] = (uint8_t)i;
            fed_row[nf] = (uint8_t)r;
            fed_slot[nf] = (uint8_t)k;
            nf++;
        }
    }
    *sel_reg = sreg;
    *in_reg = ireg;
    *n_rows = n;
    *in_mask = mask;
    n_fed = (uint8_t)nf;
    return 1;
}

void vpin_scan_hw(int on)
{
    fed_on = (uint8_t)(on && n_fed);
}

/* One frame: each fed row's columns into its matrix's cache. Called from the
 * platform's scan interrupt, like vpin_scan_matrices. */
void vpin_scan_feed(const uint16_t *frame)
{
    for (uint32_t f = 0; f < n_fed; f++) {
        uint32_t i = fed_hub[f];
        const gm_hub_t *h = &gm_device.hubs[i];
        uint32_t all = h->n_cols >= 32u ? 0xFFFFFFFFu : (1u << h->n_cols) - 1u;
        uint32_t bit = fed_row[f] * h->n_cols;
        uint32_t row = (((uint32_t)frame[fed_slot[f]] >> sm_shift[i]) & all) ^ sm_inv[i];
        uint32_t v = (hs[i].in & ~(all << bit)) | (row << bit);
        hs[i].in = v;
        hs[i].u.sm.seen_hi |= v;
        hs[i].u.sm.seen_lo |= ~v;
    }
}

__attribute__((weak)) uint32_t hal_irq_save(void)
{
    return 0;
}

__attribute__((weak)) void hal_irq_restore(uint32_t state)
{
    (void)state;
}

/* What every switch matrix read since the last take, and start the next
 * span from the levels they have now (a level held across the take is in
 * both spans). Frames only come in when a row changed, so the levels now
 * stand for the frames since. */
void vpin_span_take(void)
{
    for (uint32_t i = 0; i < gm_device.n_hubs; i++) {
        if (gm_device.hubs[i].type != GM_HUB_SWMATRIX)
            continue;
        hub_state_t *s = &hs[i];
        uint32_t irq = hal_irq_save();
        uint32_t v = s->in;
        s->u.sm.span_hi = s->u.sm.seen_hi | v;
        s->u.sm.span_lo = s->u.sm.seen_lo | ~v;
        s->u.sm.seen_hi = v;
        s->u.sm.seen_lo = ~v;
        hal_irq_restore(irq);
    }
}

int vpin_read_span(gm_pin_t p, int *any)
{
    const gm_hub_t *h = p.hub ? hub_of(p) : 0;
    if (!h || h->type != GM_HUB_SWMATRIX || p.num >= 32u) {
        int v = vpin_read(p);
        *any = v;
        return v;
    }
    const hub_state_t *s = &hs[p.hub - 1];
    int hi = (int)((s->u.sm.span_hi >> p.num) & 1u), lo = (int)((s->u.sm.span_lo >> p.num) & 1u);
    if (p.flags & GM_PIN_INVERT) {
        *any = lo;
        return !hi;
    }
    *any = hi;
    return !lo;
}

/* ---- TM1628 LED drivers ------------------------------------------------ */

#if GM_TM1628_RAM > 0
/* Bus timing, from the datasheet: CLK <= 1 MHz, PWCLK >= 400 ns high and
 * low, DIO setup and hold >= 100 ns around the CLK rising edge, PWSTB
 * >= 1 us, CLK rising to STB rising >= 1 us. DIO is set at the start of the
 * CLK low phase, so TM_CLK_LOW_NS is also its setup time: an open-drain DIO
 * (the Code 61's) has to rise through its pull-up within it (R * C: 10 kOhm
 * and 30 pF is 300 ns to 63 %, 360 ns to the TM1628's 0.7 VDD). Raise it if
 * LEDs or the display show garbage. Each phase is counted on hal_cycles()
 * from just after its edge, so the code around only adds to it. Without a
 * cycle counter (hal_ns_to_cycles 0) or set/reset registers, each phase is
 * hal_delay_us(1), as before. */
#ifndef TM_CLK_LOW_NS
#define TM_CLK_LOW_NS 600u
#endif
#ifndef TM_CLK_HIGH_NS
#define TM_CLK_HIGH_NS 450u
#endif
#define TM_STB_US 1u                /* STB low to the first bit, last bit to STB high, STB high */

__attribute__((weak)) uint32_t hal_ns_to_cycles(uint32_t ns)
{
    (void)ns;
    return 0;
}

/* CLK, DIO and STB of a TM1628 hub: a set/reset register store each
 * (hal_pin_bsrr), or hal_pin_write where the platform has none. Looked up
 * when a chip is written (a few calls, against ~100 us of bus time), so
 * it takes no RAM. */
#define TM_CLK 0u
#define TM_DIO 1u
#define TM_STB 2u
typedef struct {
    volatile uint32_t *reg[3];      /* 0 = hal_pin_write(pin[k]) */
    uint16_t pin[3];
    uint16_t set[3];                /* 1 << bit; clear = set << 16 */
    uint32_t low, high;             /* CLK phases in hal_cycles(), 0 = none: hal_delay_us */
} tm_io_t;

#ifdef GM_TEST_STORE                /* host tests: see each store (test_tm1628.c) */
void gm_test_store(volatile uint32_t *reg, uint32_t w);
#define TM_STORE(reg, w) gm_test_store(reg, w)
#else
#define TM_STORE(reg, w) (*(reg) = (w))
#endif

static void tm_io_init(tm_io_t *io, const gm_hub_t *h)
{
    const gm_pin_t p[3] = { h->clock, h->data, h->latch };
    volatile uint32_t *reg[3];
    uint32_t bit[3], fast = 1;
    for (uint32_t k = 0; k < 3u; k++) {
        bit[k] = 0;
        reg[k] = p[k].hub ? 0 : hal_pin_bsrr(p[k].num, &bit[k]);
        fast &= reg[k] != 0 && bit[k] < 16u;
        io->pin[k] = p[k].num;
    }
    for (uint32_t k = 0; k < 3u; k++) {     /* all three fast, or none */
        io->reg[k] = fast ? reg[k] : 0;
        io->set[k] = (uint16_t)(1u << (bit[k] & 15u));
    }
    io->low = hal_ns_to_cycles(TM_CLK_LOW_NS);
    io->high = io->low ? hal_ns_to_cycles(TM_CLK_HIGH_NS) : 0;
}

static inline __attribute__((always_inline)) void tm_pin(const tm_io_t *io, uint32_t k, uint32_t level)
{
    if (io->reg[k])
        TM_STORE(io->reg[k], level ? io->set[k] : (uint32_t)io->set[k] << 16);
    else
        hal_pin_write(io->pin[k], (int)level);
}

/* One byte, LSB first: DIO is set while CLK is low and taken on the rising
 * edge. With set/reset stores and a cycle counter (the STM32) the loop keeps
 * everything in registers: about 1.3 us a bit at 72 MHz, the two counted
 * phases plus ~15 cycles. Otherwise hal_pin_write and 1 us a phase. */
static void tm_byte(const tm_io_t *io, uint32_t v)
{
    if (io->reg[TM_CLK] && io->low) {
        volatile uint32_t *clk = io->reg[TM_CLK], *dio = io->reg[TM_DIO];
        uint32_t c1 = io->set[TM_CLK], c0 = c1 << 16, d1 = io->set[TM_DIO], d0 = d1 << 16;
        uint32_t low = io->low, high = io->high;
        for (uint32_t b = 0; b < 8u; b++, v >>= 1) {
            uint32_t d = (v & 1u) ? d1 : d0;
            TM_STORE(clk, c0);
            TM_STORE(dio, d);
            uint32_t t = hal_cycles();
            while (hal_cycles() - t < low) {
            }
            TM_STORE(clk, c1);
            t = hal_cycles();
            while (hal_cycles() - t < high) {
            }
        }
        return;
    }
    for (uint32_t b = 0; b < 8u; b++) {
        tm_pin(io, TM_CLK, 0);
        tm_pin(io, TM_DIO, (v >> b) & 1u);
        hal_delay_us(1);
        tm_pin(io, TM_CLK, 1);
        hal_delay_us(1);
    }
}

static void tm_start(const tm_io_t *io)
{
    tm_pin(io, TM_STB, 0);
    hal_delay_us(TM_STB_US);
}

/* STB high ends a command: 1 us after the last CLK rising edge, and PWSTB
 * >= 1 us before the next one. */
static void tm_end(const tm_io_t *io)
{
    hal_delay_us(TM_STB_US);
    tm_pin(io, TM_STB, 1);
    tm_pin(io, TM_DIO, 1);
    hal_delay_us(TM_STB_US);
}

static void tm_cmd(const tm_io_t *io, uint32_t cmd)
{
    tm_start(io);
    tm_byte(io, cmd);
    tm_end(io);
}

/* Write what changed in each chip, at most `budget` bytes on the bus per
 * call; what's left stays dirty for the next call. A chip's first write is
 * its whole RAM plus the display mode and control commands (they stay set in
 * the chip). After that: the data command 0x40 (write, auto-increment), then
 * each run of changed bytes as one command, 0xC0 | its first address and its
 * bytes. A run goes on over one unchanged byte (rewriting it costs a bus
 * byte, as a new address command would, and saves the strobe) when the
 * budget has room for the changed byte after it too; two or more end it.
 * A single changed byte is 0x40, C0|a, byte: 3 bus bytes, as with a fixed
 * address. A run the budget cuts short goes on at its next address
 * (a new 0xC0 | address) on the next call. */
static uint32_t tm1628_send(const gm_hub_t *h, hub_state_t *s, uint32_t budget)
{
    if (!s->u.tm.dirty)
        return budget;
    uint32_t n = h->ram_bytes;
    if (s->ram + n > GM_TM1628_RAM)
        n = GM_TM1628_RAM - s->ram;
    uint8_t *ram = &hub_ram[s->ram], *sent = &ram_sent[s->ram];
    tm_io_t bus;
    const tm_io_t *io = &bus;
    tm_io_init(&bus, h);
    uint32_t ctl = s->u.tm.ctl ? s->u.tm.ctl : 0x88u | (h->brightness & 7u);
    if (!s->never && ctl != s->u.tm.ctl_sent) {    /* a new brightness (fade:) */
        tm_cmd(&bus, ctl);
        s->u.tm.ctl_sent = (uint8_t)ctl;
        budget--;
    }
    uint32_t k = 0, open = 0, left = budget;
    if (s->never) {                         /* all of it now, whatever the budget */
        if (h->grids >= 4u && h->grids <= 7u)
            tm_cmd(io, h->grids - 4u);
        for (k = 0; k < n; k++)
            sent[k] = (uint8_t)~ram[k];     /* every byte "changed": one run */
        k = 0;
        budget = 0xFFFFFFFFu;
    }
    for (;;) {
        while (k < n && ram[k] == sent[k])
            k++;
        if (k == n || budget < 3u - open)   /* 0x40 once, then C0|a and a byte */
            break;
        if (!open) {
            tm_cmd(io, 0x40u);
            budget--;
            open = 1;
        }
        tm_start(io);
        tm_byte(io, 0xC0u | k);
        budget--;
        do {
            tm_byte(io, ram[k]);
            sent[k] = ram[k];
            k++;
            budget--;
        } while (budget && k < n &&
                 (ram[k] != sent[k] || (budget >= 2u && k + 1u < n && ram[k + 1u] != sent[k + 1u])));
        tm_end(io);
    }
    s->u.tm.dirty = k < n;
    if (s->never) {
        tm_cmd(io, ctl);
        s->u.tm.ctl_sent = (uint8_t)ctl;
        s->never = 0;
        budget = left > 3u + n ? left - 3u - n : 0u;
    }
    return budget;
}

static void tm1628_init(const gm_hub_t *h, hub_state_t *s)
{
    /* Idle: STB, CLK and DIO high. RAM all off, written on the first send
     * (s->never: the whole RAM). */
    mcu_out(h->latch, 1);
    mcu_out(h->clock, 1);
    mcu_out(h->data, 1);
    s->ram = ram_alloc(h->ram_bytes);
    s->u.tm.dirty = 1;
    for (uint32_t k = 0; k < h->ram_bytes && s->ram + k < GM_TM1628_RAM; k++)
        hub_ram[s->ram + k] = 0;
}

/* Pin num = RAM bit, byte * 8 + bit. */
static void tm1628_write(const gm_hub_t *h, hub_state_t *s, uint32_t num, int level)
{
    uint32_t k = num >> 3u;
    if (k >= h->ram_bytes || s->ram + k >= GM_TM1628_RAM)
        return;
    uint8_t *b = &hub_ram[s->ram + k], m = (uint8_t)(1u << (num & 7u));
    uint8_t v = level ? (uint8_t)(*b | m) : (uint8_t)(*b & ~m);
    if (v != *b) {
        *b = v;
        s->u.tm.dirty = 1;
    }
}

#if GM_TM_FADE
/* How bright each step looks, 0..255: the chip's pulse widths (1, 2, 4,
 * 10, 11, 12, 13, 14 sixteenths) through gamma 2.2. */
static const uint8_t tm_seen[8] = { 72, 99, 136, 206, 215, 224, 232, 240 };

int vpin_fades(gm_pin_t p)
{
    const gm_hub_t *h = p.hub ? hub_of(p) : 0;
    return h && h->type == GM_HUB_TM1628 && h->fade;
}

void vpin_fade(gm_pin_t p, uint32_t v, int colour)
{
    if (!vpin_fades(p))
        return;
    uint16_t *peak = &hs[p.hub - 1].u.tm.peak[colour ? 1 : 0];
    if (v > 255u)
        v = 255u;
    if (v + 1u > *peak)
        *peak = (uint16_t)(v + 1u);
}

/* The step that looks most like v of full (the hub's intensity); darker
 * than half the dimmest step: display off. */
static uint32_t tm_fade_ctl(const gm_hub_t *h, uint32_t v)
{
    uint32_t top = h->brightness & 7u, want = v * tm_seen[top] / 255u, best = 0;
    if (2u * want < tm_seen[0])
        return 0x80u;
    for (uint32_t k = 1; k <= top; k++) {
        uint32_t d = want > tm_seen[k] ? want - tm_seen[k] : tm_seen[k] - want;
        uint32_t e = want > tm_seen[best] ? want - tm_seen[best] : tm_seen[best] - want;
        if (d < e)
            best = k;
    }
    return 0x88u | best;
}

void vpin_fade_apply(void)
{
    for (uint32_t i = 0; i < gm_device.n_hubs; i++) {
        const gm_hub_t *h = &gm_device.hubs[i];
        hub_state_t *s = &hs[i];
        if (h->type != GM_HUB_TM1628)
            continue;
        /* a host's colours win: an LED it turned off shows its idle look,
         * which mustn't hold the chip at full */
        uint32_t peak = s->u.tm.peak[1] ? s->u.tm.peak[1] : s->u.tm.peak[0];
        s->u.tm.peak[0] = s->u.tm.peak[1] = 0;
        if (!h->fade || !peak)
            continue;
        uint32_t ctl = tm_fade_ctl(h, peak - 1u);
        if (ctl != (s->u.tm.ctl ? s->u.tm.ctl : (0x88u | (h->brightness & 7u)))) {
            s->u.tm.ctl = (uint8_t)ctl;
            s->u.tm.dirty = 1;
        }
    }
}
#else
int vpin_fades(gm_pin_t p)
{
    (void)p;
    return 0;
}

void vpin_fade(gm_pin_t p, uint32_t v, int colour)
{
    (void)p;
    (void)v;
    (void)colour;
}

void vpin_fade_apply(void)
{
}
#endif

/* ---- RGB_SERIAL hubs --------------------------------------------------- */

/* The M-Audio Code pad LED board (found in the Code 61 v1.2.2 app, 0x080115FC
 * and 0x08013C94): one LED per message, 02, 0x80 + LED, R, G, B, 03, with
 * 00 off and FF full; a lone A5 keepalive about every 60 ms (without it the
 * board turns its LEDs off). Messages sent back to back are lost after the
 * first, so every write, the A5 included, goes out RGB_GAP_MS after the
 * last one. */
#define RGB_GAP_MS 5u
#define RGB_KEEPALIVE_MS 50u
/* At power-on the board isn't listening yet: the first colours are lost
 * (the pads stayed dark until hit). So every LED is sent once more,
 * RGB_REPAINT_MS after start; after that an LED goes out only when it
 * changes. Resending one LED every 50 ms forever (builds 74-93) left the
 * board deaf after a cold start: with no LED message during its start-up
 * it comes up fine (tested on the Code 61, 2026-11-09). */
#ifndef RGB_REPAINT_MS
#define RGB_REPAINT_MS 2000u
#endif
/* With rx_pin: the board answers each LED message with 0x40 + LED (the
 * Code 61's does, about 220 ms after power-on at the earliest). An LED is
 * only counted as shown once answered: without an answer in RGB_ACK_MS it
 * is sent again, after a pause that grows with each miss (RGB_RETRY_MS
 * times the misses in a row, at most RGB_RETRY_MAX_MS), so a board that
 * is still starting gets little traffic. The first answer means the board
 * is up: every LED is sent again then, instead of at RGB_REPAINT_MS. With
 * no answer by RGB_ACK_GIVE_UP_MS the hub works as without rx_pin. */
#define RGB_ACK_MS 20u
#define RGB_RETRY_MS 50u
#define RGB_RETRY_MAX_MS 500u
#define RGB_ACK_GIVE_UP_MS 3000u
#define RGB_ACK 0x40u

static void rgb_serial_init(const gm_hub_t *h, hub_state_t *s)
{
    /* All off; every LED is sent once (ram_sent starts different). */
    s->ram = ram_alloc(h->ram_bytes);
    for (uint32_t k = 0; k < h->ram_bytes && s->ram + k < GM_TM1628_RAM; k++) {
        hub_ram[s->ram + k] = 0;
        ram_sent[s->ram + k] = 0xFFu;
    }
    if (h->data.hub == 0)
        (void)hal_serial_open(h->data.num, 115200u, (h->dma ? HAL_UART_DMA : 0u) |
                              (vpin_is_none(h->latch) ? 0u : HAL_UART_RX));
}

/* Pin num = LED * 3 + colour: on is 255, off 0. */
static void rgb_serial_write(const gm_hub_t *h, hub_state_t *s, uint32_t num, int level)
{
    if (num < h->ram_bytes && s->ram + num < GM_TM1628_RAM)
        hub_ram[s->ram + num] = level ? 0xFFu : 0u;
}

static int rgb_serial_rgb(const gm_hub_t *h, hub_state_t *s, uint32_t num, uint32_t rgb)
{
    if (num % 3u || num + 3u > h->ram_bytes || s->ram + num + 3u > GM_TM1628_RAM)
        return 0;
    uint8_t *b = &hub_ram[s->ram + num];
    b[0] = (uint8_t)(rgb >> 16);
    b[1] = (uint8_t)(rgb >> 8);
    b[2] = (uint8_t)rgb;
    return 1;
}

/* LED led's three bytes differ between a and b. */
static int rgb_differs(const uint8_t *a, const uint8_t *b, uint32_t led)
{
    a += led * 3u;
    b += led * 3u;
    return a[0] != b[0] || a[1] != b[1] || a[2] != b[2];
}

/* What goes on the wire for colour c (0 red, 1 green, 2 blue) at value v:
 * through the hub's curve (gamma:), then scaled to its white (white:). A
 * value above 0 stays above 0. */
static uint8_t rgb_out(const gm_hub_t *h, uint32_t c, uint32_t v)
{
    if (h->rgb_curve)
        v = h->rgb_curve[v];
    uint32_t w = h->rgb_white[c] ? h->rgb_white[c] : 255u;
    if (w == 255u || !v)
        return (uint8_t)v;
    v = (v * w + 127u) / 255u;
    return (uint8_t)(v ? v : 1u);
}

/* LED led no longer counts as sent: it goes out again. */
static void rgb_unsend(const uint8_t *ram, uint8_t *sent, uint32_t led)
{
    for (uint32_t c = 0; c < 3u; c++)
        sent[led * 3u + c] = (uint8_t)~ram[led * 3u + c];
}

/* Send one changed LED (at most every RGB_GAP_MS) and the keepalive. */
static void rgb_serial_poll(const gm_hub_t *h, hub_state_t *s, uint32_t now_ms)
{
    if (h->data.hub != 0 || s->ram + h->ram_bytes > GM_TM1628_RAM)
        return;
    uint16_t tx = h->data.num;
    const uint8_t *ram = &hub_ram[s->ram];
    uint8_t *sent = &ram_sent[s->ram];
    int use_acks = !vpin_is_none(h->latch) && s->u.rgb.acks != 2u;
    int repaint = 0;
    if (use_acks) {
        for (int b; (b = hal_serial_read(tx)) >= 0;) {
            uint32_t led = (uint32_t)b - RGB_ACK;
            if (led >= h->chain)
                continue;               /* not an answer */
            if (!s->u.rgb.acks) {
                s->u.rgb.acks = 1;
                repaint = !s->u.rgb.repainted;   /* the board is up: everything again */
            }
            if (s->u.rgb.wait_led == led + 1u) {
                s->u.rgb.wait_led = 0;
                s->u.rgb.misses = 0;
            }
        }
        if (s->u.rgb.wait_led && now_ms - s->u.rgb.t_led >= RGB_ACK_MS) {
            rgb_unsend(ram, sent, s->u.rgb.wait_led - 1u);     /* not answered: again */
            s->u.rgb.wait_led = 0;
            if (s->u.rgb.misses < 255u)
                s->u.rgb.misses++;
        }
        if (!s->u.rgb.acks && now_ms >= RGB_ACK_GIVE_UP_MS) {
            s->u.rgb.acks = 2;          /* never answered: as without rx_pin */
            s->u.rgb.wait_led = 0;
            s->u.rgb.misses = 0;
            use_acks = 0;
        }
    }
    if (!s->u.rgb.repainted && (repaint || (!use_acks && now_ms >= RGB_REPAINT_MS))) {
        for (uint32_t k = 0; k < h->ram_bytes; k++)
            sent[k] = (uint8_t)~ram[k];     /* every LED differs: all sent again */
        s->u.rgb.repainted = 1;
    }
    if (now_ms - s->u.rgb.t_any < RGB_GAP_MS)
        return;
    if (now_ms - s->u.rgb.t_ka >= RGB_KEEPALIVE_MS) {
        static const uint8_t ka = 0xA5u;
        if (hal_serial_write(tx, &ka, 1))
            s->u.rgb.t_ka = s->u.rgb.t_any = now_ms;
        return;
    }
    if (use_acks && s->u.rgb.wait_led)
        return;                         /* the last LED isn't answered yet */
    if (use_acks && s->u.rgb.misses) {
        uint32_t pause = RGB_RETRY_MS * s->u.rgb.misses;
        if (now_ms - s->u.rgb.t_led < (pause < RGB_RETRY_MAX_MS ? pause : RGB_RETRY_MAX_MS))
            return;
    }
    /* The next changed LED after the last one sent, so one busy LED can't
     * starve the others. */
    uint32_t todo = h->chain;           /* an LED to send, h->chain = none */
    for (uint32_t k = 0; k < h->chain; k++) {
        uint32_t led = ((uint32_t)(uint8_t)s->channel + 1u + k) % h->chain;
        if (rgb_differs(ram, sent, led)) {
            todo = led;
            break;
        }
    }
    if (todo == h->chain)
        return;
    const uint8_t *v = &ram[todo * 3u];
    uint8_t msg[6] = { 0x02u, (uint8_t)(0x80u + todo), rgb_out(h, 0, v[0]),
                       rgb_out(h, 1, v[1]), rgb_out(h, 2, v[2]), 0x03u };
    if (hal_serial_write(tx, msg, sizeof msg)) {
        for (uint32_t c = 0; c < 3u; c++)
            sent[todo * 3u + c] = v[c];
        s->channel = (int8_t)todo;
        s->u.rgb.t_any = s->u.rgb.t_led = now_ms;
        if (use_acks)
            s->u.rgb.wait_led = (uint8_t)(todo + 1u);
    }
}
#else
int vpin_fades(gm_pin_t p)
{
    (void)p;
    return 0;
}

void vpin_fade(gm_pin_t p, uint32_t v, int colour)
{
    (void)p;
    (void)v;
    (void)colour;
}

void vpin_fade_apply(void)
{
}
#endif

/* ---- I2C hubs (main loop) --------------------------------------------- */

#define MCP_IODIRA 0x00
#define MCP_GPPUA  0x0C
#define MCP_GPIOA  0x12
#define MCP_OLATA  0x14

static void mcp_write16(uint8_t addr, uint8_t reg, uint32_t v)
{
    uint8_t b[3] = { reg, (uint8_t)v, (uint8_t)(v >> 8) };
    hal_i2c_write(addr, b, 3);
}

static void i2c_init(const gm_hub_t *h, hub_state_t *s)
{
    (void)h;
    s->u.io.cfg_dirty = 1;
}

static void i2c_input(const gm_hub_t *h, hub_state_t *s, gm_pin_t p)
{
    s->u.io.dir_out &= ~(1u << p.num);
    s->u.io.in_used |= 1u << p.num;
    if (p.flags & GM_PIN_PULLUP)
        s->u.io.pullup |= 1u << p.num;
    if (h->type == GM_HUB_PCF8574)
        s->out |= 1u << p.num;          /* quasi-bidirectional: input = high */
    s->u.io.cfg_dirty = 1;
}

static void i2c_output(const gm_hub_t *h, hub_state_t *s, gm_pin_t p)
{
    (void)h;
    s->u.io.dir_out |= 1u << p.num;
    s->u.io.cfg_dirty = 1;
}

/* A chip nothing reads (only LEDs on it) isn't read: an MCP23017 read is
 * two transactions, ~120 us at 400 kHz. */
static void mcp23017_refresh(const gm_hub_t *h, hub_state_t *s)
{
    uint8_t b[2], reg = MCP_GPIOA;
    if (s->u.io.in_used && hal_i2c_write(h->i2c_addr, &reg, 1) == 0 && hal_i2c_read(h->i2c_addr, b, 2) == 0)
        s->in = b[0] | (uint32_t)b[1] << 8;
}

static void mcp23017_flush(const gm_hub_t *h, hub_state_t *s)
{
    if (s->u.io.cfg_dirty) {
        mcp_write16(h->i2c_addr, MCP_IODIRA, ~s->u.io.dir_out & 0xFFFFu);
        mcp_write16(h->i2c_addr, MCP_GPPUA, s->u.io.pullup & 0xFFFFu);
        s->u.io.cfg_dirty = 0;
        s->never = 1;
    }
    if (s->out != s->written || s->never) {
        mcp_write16(h->i2c_addr, MCP_OLATA, s->out);
        s->written = s->out;
        s->never = 0;
    }
}

static void pcf8574_refresh(const gm_hub_t *h, hub_state_t *s)
{
    uint8_t b;
    if (s->u.io.in_used && hal_i2c_read(h->i2c_addr, &b, 1) == 0)
        s->in = b;
}

static void pcf8574_flush(const gm_hub_t *h, hub_state_t *s)
{
    /* Inputs must stay written high. */
    uint32_t v = (s->out | ~s->u.io.dir_out) & 0xFFu;
    if (v != s->written || s->never) {
        uint8_t b = (uint8_t)v;
        hal_i2c_write(h->i2c_addr, &b, 1);
        s->written = v;
        s->never = 0;
    }
}

/* ---- the table: one gm_hub_ops_t per hub type -------------------------- */

static const gm_hub_ops_t hc595_ops = {
    .flags = GM_HUBF_IN_REG, .init = hc595_init, .flush = hc595_flush,
};
static const gm_hub_ops_t hc165_ops = {
    .flags = GM_HUBF_IN_REG, .init = hc165_init, .refresh = hc165_refresh,
};
static const gm_hub_ops_t mux_ops = {
    .init = mux_init, .analog = mux_analog,         /* digital reads: vpin_read */
};
static const gm_hub_ops_t mcp23017_ops = {
    .flags = GM_HUBF_BUS | GM_HUBF_IN_REG, .init = i2c_init, .input = i2c_input, .output = i2c_output,
    .refresh = mcp23017_refresh, .flush = mcp23017_flush,
};
static const gm_hub_ops_t pcf8574_ops = {
    .flags = GM_HUBF_BUS | GM_HUBF_IN_REG, .init = i2c_init, .input = i2c_input, .output = i2c_output,
    .refresh = pcf8574_refresh, .flush = pcf8574_flush,
};
static const gm_hub_ops_t decoder_ops = {
    .flags = GM_HUBF_SELECT, .init = decoder_init,  /* writes: vpin_write */
};
static const gm_hub_ops_t swmatrix_ops = {
    .flags = GM_HUBF_IN_REG | GM_HUBF_MATRIX, .init = swmatrix_init,   /* scan: vpin_scan_matrices */
};
#if GM_TM1628_RAM > 0
static const gm_hub_ops_t tm1628_ops = {
    .flags = GM_HUBF_NO_READ, .init = tm1628_init, .write = tm1628_write, .send = tm1628_send,
};
static const gm_hub_ops_t rgb_serial_ops = {
    .flags = GM_HUBF_NO_READ, .init = rgb_serial_init, .write = rgb_serial_write, .rgb = rgb_serial_rgb,
    .poll = rgb_serial_poll,
};
#endif
static const gm_hub_ops_t xy_ops = {
    .flags = GM_HUBF_IN_REG, .init = xy_init, .analog = xy_analog,
};
static const gm_hub_ops_t no_ops = { 0 };

#define HUB(t, ops) [t] = USES(t) ? &(ops) : 0
static const gm_hub_ops_t *const hub_ops[] = {
    HUB(GM_HUB_HC595, hc595_ops),
    HUB(GM_HUB_HC165, hc165_ops),
    HUB(GM_HUB_MUX, mux_ops),
    HUB(GM_HUB_MCP23017, mcp23017_ops),
    HUB(GM_HUB_PCF8574, pcf8574_ops),
    HUB(GM_HUB_DECODER, decoder_ops),
    HUB(GM_HUB_SWMATRIX, swmatrix_ops),
#if GM_TM1628_RAM > 0
    HUB(GM_HUB_TM1628, tm1628_ops),
    HUB(GM_HUB_RGB_SERIAL, rgb_serial_ops),
#endif
    HUB(GM_HUB_XY, xy_ops),
};

static const gm_hub_ops_t *hub_ops_of(uint32_t type)
{
    const gm_hub_ops_t *o = type < sizeof hub_ops / sizeof hub_ops[0] ? hub_ops[type] : 0;
    return o ? o : &no_ops;
}
