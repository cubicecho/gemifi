/*
 * vpin.c: virtual pins. See gm_vpin.h.
 *
 * Hub control pins (data, clock, latch, select, common, enable) must be MCU
 * pins (hub 0). /E of a mux and /OE of a 595 are driven low (enabled).
 * TM1628 hubs keep their display RAM in tm_ram (GM_TM1628_RAM bytes, all
 * TM1628 hubs back to back), since a chip has more than 32 bits. RGB_SERIAL
 * hubs keep their LED bits there too (bit = LED * 3 + colour).
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_vpin.h"
#include "gm_hal.h"
#include "gm_device_config.h"
#include "gm_controller.h"
#include "app_proto.h"

#define N_HUBS (GM_N_HUBS > 0 ? GM_N_HUBS : 1)
#ifndef GM_TM1628_RAM
#define GM_TM1628_RAM 0             /* no TM1628 / RGB_SERIAL hub */
#endif
/* Hubs whose output pins are bits in tm_ram. */
#define RAM_HUB(t) ((t) == GM_HUB_TM1628 || (t) == GM_HUB_RGB_SERIAL)

typedef struct {
    uint32_t in;          /* last read, electrical levels, bit = hub pin */
    uint32_t out;         /* wanted output levels                         */
    uint32_t written;     /* what the chip has                            */
    uint32_t dir_out;     /* MCP/PCF: pins used as outputs                */
    uint32_t pullup;      /* MCP: pins with the internal pull-up          */
    uint8_t cfg_dirty;    /* MCP: IODIR/GPPU need writing                 */
    uint8_t never;        /* outputs never written yet                    */
    int8_t channel;       /* MUX: selected channel, -1 = none             */
    uint8_t tm_dirty;     /* TM1628: RAM changed since the last write     */
    uint16_t tm_off;      /* TM1628, RGB_SERIAL: its bytes in tm_ram      */
} hub_state_t;

static hub_state_t hs[N_HUBS];

/* Decoders whose address and enable lines are all on one port with a set/reset
 * register (hal_pin_bsrr): each output is one precomputed store. */
#define DEC_FAST 2u                 /* decoders with a table */
#define DEC_OUT 32u                 /* outputs per table */
static int8_t dec_slot[N_HUBS];     /* table index, -1 = write pin by pin */
static volatile uint32_t *dec_reg[DEC_FAST];
static uint32_t dec_word[DEC_FAST][DEC_OUT];
static uint8_t dec_used;

__attribute__((weak)) int hal_serial_open(uint16_t tx_pin, uint32_t baud)
{
    (void)tx_pin;
    (void)baud;
    return 0;
}

__attribute__((weak)) int hal_serial_write(uint16_t tx_pin, const uint8_t *b, uint32_t n)
{
    (void)tx_pin;
    (void)b;
    (void)n;
    return 0;
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
static uint8_t tm_ram[GM_TM1628_RAM];
static uint8_t ram_sent[GM_TM1628_RAM];   /* what the chip / LED board has (its bytes of tm_ram) */
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
    const gm_hub_t *h = hub_of(p);
    return h && (h->type == GM_HUB_MCP23017 || h->type == GM_HUB_PCF8574);
}

static void mcu_out(gm_pin_t p, int level)
{
    if (p.hub == 0) {
        hal_pin_write(p.num, level);    /* first too: no glitch where it can */
        hal_pin_mode(p.num, (p.flags & GM_PIN_OPEN_DRAIN) ? HAL_OUT_OD : HAL_OUT);
        hal_pin_write(p.num, level);
    }
}

static void mux_shared_init(void);

/* Hardware scan (vpin_scan_plan): which plan slot each switch-matrix row
 * reads from. */
#define FED_MAX 32u
static uint8_t fed_hub[FED_MAX], fed_row[FED_MAX], fed_slot[FED_MAX];
static uint8_t n_fed, fed_on;
static void adc_off(void);          /* the hardware analog scan, below */

void vpin_init(void)
{
#if GM_TM1628_RAM > 0
    uint32_t tm_off = 0;
#endif
    n_fed = 0;
    fed_on = 0;
    adc_off();
    for (uint32_t i = 0; i < gm_device.n_hubs; i++) {
        const gm_hub_t *h = &gm_device.hubs[i];
        hub_state_t *s = &hs[i];
        s->in = 0;
        s->out = 0;
        s->written = 0;
        s->dir_out = 0;
        s->pullup = 0;
        s->cfg_dirty = 1;
        s->never = 1;
        s->channel = -1;
        s->tm_dirty = 0;
        s->tm_off = 0;
        dec_slot[i] = -1;
        switch (h->type) {
        case GM_HUB_HC595:
            mcu_out(h->data, 0);
            mcu_out(h->clock, 0);
            mcu_out(h->latch, 0);
            mcu_out(h->enable, 0);
            break;
        case GM_HUB_HC165:
            mcu_out(h->clock, 0);
            mcu_out(h->latch, 1);
            if (h->data.hub == 0)
                hal_pin_mode(h->data.num, HAL_IN);
            break;
        case GM_HUB_DECODER:
            for (uint32_t k = 0; k < h->n_sel && k < 4; k++)
                mcu_out(h->sel[k], 0);
            for (uint32_t k = 0; k < h->n_en && k < 3; k++)
                mcu_out(h->en[k], 0);
            dec_fast_init(i, h);
            break;
        case GM_HUB_SWMATRIX:
            for (uint32_t c = 0; c < h->n_cols; c++)
                vpin_input(h->cols[c]);
            for (uint32_t r = 0; r < h->n_rows; r++)
                vpin_output(h->rows[r]);
            sm_fast_init(i, h);
            break;
        case GM_HUB_MUX:
            for (uint32_t k = 0; k < h->n_sel && k < 4; k++)
                mcu_out(h->sel[k], 0);
            mcu_out(h->enable, 0);
            if (h->common.hub == 0)
                hal_pin_mode(h->common.num,
                             (h->common.flags & GM_PIN_PULLUP) ? HAL_IN_PULLUP :
                             (h->common.flags & GM_PIN_PULLDOWN) ? HAL_IN_PULLDOWN : HAL_IN);
            break;
#if GM_TM1628_RAM > 0
        case GM_HUB_TM1628:
            /* Idle: STB, CLK and DIO high. RAM all off, written on the
             * first vpin_flush_tm1628(). */
            mcu_out(h->latch, 1);
            mcu_out(h->clock, 1);
            mcu_out(h->data, 1);
            s->tm_off = (uint16_t)tm_off;
            s->tm_dirty = 1;            /* and never = 1: the first write is the whole RAM */
            for (uint32_t k = 0; k < h->ram_bytes && tm_off < GM_TM1628_RAM; k++)
                tm_ram[tm_off++] = 0;
            break;
        case GM_HUB_RGB_SERIAL:
            /* All off; every LED is sent once (ram_sent starts different). */
            s->tm_off = (uint16_t)tm_off;
            for (uint32_t k = 0; k < h->ram_bytes && tm_off < GM_TM1628_RAM; k++) {
                tm_ram[tm_off] = 0;
                ram_sent[tm_off++] = 0xFFu;
            }
            if (h->data.hub == 0)
                (void)hal_serial_open(h->data.num, 115200u);
            break;
#endif
        default:
            break;
        }
    }
    mux_shared_init();
}

void vpin_input(gm_pin_t p)
{
    if (p.hub == 0) {
        hal_pin_mode(p.num, (p.flags & GM_PIN_PULLUP) ? HAL_IN_PULLUP :
                            (p.flags & GM_PIN_PULLDOWN) ? HAL_IN_PULLDOWN : HAL_IN);
        return;
    }
    const gm_hub_t *h = hub_of(p);
    if (!h || RAM_HUB(h->type))
        return;
    hub_state_t *s = &hs[p.hub - 1];
    s->dir_out &= ~(1u << p.num);
    if (p.flags & GM_PIN_PULLUP)
        s->pullup |= 1u << p.num;
    if (h->type == GM_HUB_PCF8574)
        s->out |= 1u << p.num;          /* quasi-bidirectional: input = high */
    s->cfg_dirty = 1;
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
    if (RAM_HUB(h->type)) {
        vpin_write(p, 0);
        return;
    }
    hs[p.hub - 1].dir_out |= 1u << p.num;
    hs[p.hub - 1].cfg_dirty = 1;
    vpin_write(p, 0);
}

/* Muxes may share select lines (allow_other_uses): switching one switches
 * the others. One with the same lines is now on the same channel; one that
 * shares only some must switch again. */
/* Which muxes share select lines with which, worked out once (mux_shared_init):
 * bit j of mux_same[i] = hub j has exactly hub i's lines (so it is now on the
 * same channel), of mux_some[i] = it shares only some (so it must switch
 * again). Masks cover hubs 0-31; a device with more falls back to the loop. */
static uint32_t mux_same[N_HUBS], mux_some[N_HUBS];
static uint8_t mux_masks;

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

static void mux_shared_init(void)
{
    mux_masks = gm_device.n_hubs <= 32u;
    for (uint32_t i = 0; mux_masks && i < gm_device.n_hubs; i++) {
        mux_same[i] = mux_some[i] = 0;
        const gm_hub_t *h = &gm_device.hubs[i];
        if (h->type != GM_HUB_MUX)
            continue;
        for (uint32_t j = 0; j < gm_device.n_hubs; j++) {
            const gm_hub_t *o = &gm_device.hubs[j];
            int any, same;
            if (j == i || o->type != GM_HUB_MUX)
                continue;
            mux_pair(h, o, &any, &same);
            if (any)
                *(same ? &mux_same[i] : &mux_some[i]) |= 1u << j;
        }
    }
}

static void mux_shared(const gm_hub_t *h, uint32_t ch)
{
    uint32_t i = (uint32_t)(h - gm_device.hubs);
    if (mux_masks) {
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
    if (RAM_HUB(h->type))
        return 0;                       /* outputs only */
    return (int)((s->in >> p.num) & 1u) ^ inv;
}

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
#if GM_TM1628_RAM > 0
    if (RAM_HUB(h->type)) {
        uint32_t k = p.num >> 3u;
        if (k >= h->ram_bytes || s->tm_off + k >= GM_TM1628_RAM)
            return;
        uint8_t *b = &tm_ram[s->tm_off + k], m = (uint8_t)(1u << (p.num & 7u));
        uint8_t v = level ? (uint8_t)(*b | m) : (uint8_t)(*b & ~m);
        if (v != *b) {
            *b = v;
            s->tm_dirty = 1;
        }
        return;
    }
#endif
    if (level)
        s->out |= 1u << p.num;
    else
        s->out &= ~(1u << p.num);
}

/* ---- hardware analog scan (gm_controller.h gm_adc_plan_t) -------------- */

#ifndef GM_HW_ADC
#define GM_HW_ADC 0                 /* a config generated before hardware_scan: */
#endif
#if GM_HW_ADC
#define ADC_CELLS (GM_ADC_MAX_STEPS * GM_ADC_MAX_PINS)
static gm_adc_plan_t aplan;
static uint8_t adc_on;
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
    if (aplan.n_pins >= GM_ADC_MAX_PINS)
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
        if (steps > GM_ADC_MAX_STEPS)
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
    for (uint32_t i = 0; ok && i < gm_device.n_analogs; i++)
        ok = adc_plan_source(gm_device.analogs[i].pin, &group);
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
    if (p.hub == 0) {
        v = hal_adc_read(p.num);
    } else {
        const gm_hub_t *h = hub_of(p);
        if (!h || h->type != GM_HUB_MUX || h->common.hub != 0)
            return -1;
        mux_select(h, &hs[p.hub - 1], p.num);
        v = hal_adc_read(h->common.num);
    }
    if (v >= 0 && (p.flags & GM_PIN_INVERT))
        v = 4095 - v;
    return v;
}

/* ---- shift registers -------------------------------------------------- */

static void pulse(uint16_t pin)
{
    hal_pin_write(pin, 1);
    hal_pin_write(pin, 0);
}

void vpin_refresh(void)
{
    for (uint32_t i = 0; i < gm_device.n_hubs; i++) {
        const gm_hub_t *h = &gm_device.hubs[i];
        if (h->type != GM_HUB_HC165)
            continue;
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
        hs[i].in = v;
    }
}

void vpin_flush(void)
{
    for (uint32_t i = 0; i < gm_device.n_hubs; i++) {
        const gm_hub_t *h = &gm_device.hubs[i];
        hub_state_t *s = &hs[i];
        if (h->type != GM_HUB_HC595 || (s->out == s->written && !s->never))
            continue;
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
}

/* ---- switch matrices --------------------------------------------------- */

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
    if (!h || h->type == GM_HUB_MUX || h->type == GM_HUB_DECODER || RAM_HUB(h->type))
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
        hs[i].in = (hs[i].in & ~(all << bit)) | (row << bit);
    }
}

/* ---- TM1628 LED drivers ------------------------------------------------ */

#if GM_TM1628_RAM > 0
/* One byte, LSB first: DIO is set while CLK is low and taken on the rising
 * edge. 1 us per half clock keeps well inside the datasheet (CLK <= 1 MHz,
 * PWCLK >= 400 ns) with a slow pull-up on an open-drain DIO. */
static void tm_byte(const gm_hub_t *h, uint32_t v)
{
    for (uint32_t b = 0; b < 8u; b++) {
        hal_pin_write(h->clock.num, 0);
        hal_pin_write(h->data.num, (v >> b) & 1u);
        hal_delay_us(1);
        hal_pin_write(h->clock.num, 1);
        hal_delay_us(1);
    }
}

static void tm_start(const gm_hub_t *h)
{
    hal_pin_write(h->latch.num, 0);
    hal_delay_us(1);
}

/* STB high ends a command; PWSTB >= 1 us before the next one. */
static void tm_end(const gm_hub_t *h)
{
    hal_pin_write(h->latch.num, 1);
    hal_pin_write(h->data.num, 1);
    hal_delay_us(1);
}

static void tm_cmd(const gm_hub_t *h, uint32_t cmd)
{
    tm_start(h);
    tm_byte(h, cmd);
    tm_end(h);
}

/* What a chip still needs, in bytes on the bus (budget units). A whole-RAM
 * write: 0x40, C0 + every byte; fixed addresses: 0x44, then C0|a + byte for
 * each changed byte. */
static uint32_t tm_changed(const hub_state_t *s, uint32_t n)
{
    uint32_t c = 0;
    for (uint32_t k = 0; k < n; k++)
        c += tm_ram[s->tm_off + k] != ram_sent[s->tm_off + k];
    return c;
}

/* Write what changed in each chip, at most `budget` bus bytes (each about
 * 17 us) per call; what's left stays dirty for the next call. A chip's first
 * write is its whole RAM plus the display mode and control commands (they
 * stay set in the chip); after that only changed bytes go out, one at a
 * time at a fixed address, or the whole RAM when that is shorter. */
static void tm_flush(uint32_t budget)
{
    for (uint32_t i = 0; i < gm_device.n_hubs && budget; i++) {
        const gm_hub_t *h = &gm_device.hubs[i];
        hub_state_t *s = &hs[i];
        if (h->type != GM_HUB_TM1628 || !s->tm_dirty)
            continue;
        uint32_t n = h->ram_bytes;
        if (s->tm_off + n > GM_TM1628_RAM)
            n = GM_TM1628_RAM - s->tm_off;
        uint8_t *ram = &tm_ram[s->tm_off], *sent = &ram_sent[s->tm_off];
        uint32_t c = s->never ? n : tm_changed(s, n);
        if (s->never || (2u + n < 1u + 2u * c && 2u + n <= budget)) {
            if (s->never && h->grids >= 4u && h->grids <= 7u)
                tm_cmd(h, h->grids - 4u);
            tm_cmd(h, 0x40u);           /* auto-increment from address 0 */
            tm_start(h);
            tm_byte(h, 0xC0u);
            for (uint32_t k = 0; k < n; k++) {
                tm_byte(h, ram[k]);
                sent[k] = ram[k];
            }
            tm_end(h);
            if (s->never)
                tm_cmd(h, 0x88u | (h->brightness & 7u));
            s->never = 0;
            s->tm_dirty = 0;
            budget = budget > 2u + n ? budget - 2u - n : 0u;
            continue;
        }
        if (c == 0) {
            s->tm_dirty = 0;
            continue;
        }
        if (budget < 3u)
            break;
        tm_cmd(h, 0x44u);               /* fixed address */
        budget--;
        for (uint32_t k = 0; k < n && budget >= 2u; k++) {
            if (ram[k] == sent[k])
                continue;
            uint8_t v = ram[k];
            tm_start(h);
            tm_byte(h, 0xC0u | k);
            tm_byte(h, v);
            tm_end(h);
            sent[k] = v;
            budget -= 2u;
            c--;
        }
        s->tm_dirty = c != 0;
    }
}

void vpin_flush_tm1628(void)
{
    tm_flush(0xFFFFFFFFu);
}

/* About 9 bus bytes: ~150 us at most per main-loop pass, and a single LED
 * change (3 bytes, ~50 us) always goes out at once. */
#define TM_POLL_BUDGET 9u

void vpin_poll_tm1628(void)
{
    tm_flush(TM_POLL_BUDGET);
}

/* ---- RGB_SERIAL hubs --------------------------------------------------- */

/* The M-Audio Code pad LED board (found in the Code 61 v1.2.2 app, 0x080115FC
 * and 0x08013C94): one LED per message, 02, 0x80 + LED, R, G, B, 03, with
 * 00 off and FF full; a lone A5 keepalive about every 60 ms (without it the
 * board turns its LEDs off). Messages sent back to back are lost after the
 * first, so they go out one per RGB_GAP_MS. */
#define RGB_GAP_MS 5u
#define RGB_KEEPALIVE_MS 50u

static uint32_t rgb_bits(const uint8_t *ram, uint32_t led)
{
    uint32_t v = 0;
    for (uint32_t c = 0; c < 3u; c++) {
        uint32_t n = led * 3u + c;
        v |= ((ram[n >> 3u] >> (n & 7u)) & 1u) << c;
    }
    return v;
}

static void rgb_set_sent(uint8_t *ram, uint32_t led, uint32_t v)
{
    for (uint32_t c = 0; c < 3u; c++) {
        uint32_t n = led * 3u + c;
        uint8_t m = (uint8_t)(1u << (n & 7u));
        ram[n >> 3u] = (v >> c) & 1u ? (uint8_t)(ram[n >> 3u] | m) : (uint8_t)(ram[n >> 3u] & ~m);
    }
}

void vpin_poll_rgb_serial(uint32_t now_ms)
{
    static uint32_t t_msg, t_ka;
    for (uint32_t i = 0; i < gm_device.n_hubs; i++) {
        const gm_hub_t *h = &gm_device.hubs[i];
        hub_state_t *s = &hs[i];
        if (h->type != GM_HUB_RGB_SERIAL || h->data.hub != 0 || s->tm_off + h->ram_bytes > GM_TM1628_RAM)
            continue;
        uint16_t tx = h->data.num;
        if (now_ms - t_ka >= RGB_KEEPALIVE_MS) {
            static const uint8_t ka = 0xA5u;
            if (hal_serial_write(tx, &ka, 1))
                t_ka = now_ms;
            continue;
        }
        if (now_ms - t_msg < RGB_GAP_MS)
            continue;
        /* The next changed LED after the last one sent, so one busy LED
         * can't starve the others. */
        const uint8_t *ram = &tm_ram[s->tm_off];
        uint8_t *sent = &ram_sent[s->tm_off];
        for (uint32_t k = 0; k < h->chain; k++) {
            uint32_t led = ((uint32_t)(uint8_t)s->channel + 1u + k) % h->chain;
            uint32_t v = rgb_bits(ram, led);
            if (v == rgb_bits(sent, led))
                continue;
            uint8_t msg[6] = { 0x02u, (uint8_t)(0x80u + led), (v & 1u) ? 0xFFu : 0u,
                               (v & 2u) ? 0xFFu : 0u, (v & 4u) ? 0xFFu : 0u, 0x03u };
            if (hal_serial_write(tx, msg, sizeof msg)) {
                rgb_set_sent(sent, led, v);
                s->channel = (int8_t)led;
                t_msg = now_ms;
            }
            break;
        }
    }
}
#else
void vpin_flush_tm1628(void)
{
}

void vpin_poll_tm1628(void)
{
}

void vpin_poll_rgb_serial(uint32_t now_ms)
{
    (void)now_ms;
}
#endif

/* ---- I2C hubs --------------------------------------------------------- */

#define MCP_IODIRA 0x00
#define MCP_GPPUA  0x0C
#define MCP_GPIOA  0x12
#define MCP_OLATA  0x14

static void mcp_write16(uint8_t addr, uint8_t reg, uint32_t v)
{
    uint8_t b[3] = { reg, (uint8_t)v, (uint8_t)(v >> 8) };
    hal_i2c_write(addr, b, 3);
}

void vpin_refresh_i2c(void)
{
    for (uint32_t i = 0; i < gm_device.n_hubs; i++) {
        const gm_hub_t *h = &gm_device.hubs[i];
        uint8_t b[2];
        if (h->type == GM_HUB_MCP23017) {
            uint8_t reg = MCP_GPIOA;
            if (hal_i2c_write(h->i2c_addr, &reg, 1) == 0 && hal_i2c_read(h->i2c_addr, b, 2) == 0)
                hs[i].in = b[0] | (uint32_t)b[1] << 8;
        } else if (h->type == GM_HUB_PCF8574) {
            if (hal_i2c_read(h->i2c_addr, b, 1) == 0)
                hs[i].in = b[0];
        }
    }
}

void vpin_flush_i2c(void)
{
    for (uint32_t i = 0; i < gm_device.n_hubs; i++) {
        const gm_hub_t *h = &gm_device.hubs[i];
        hub_state_t *s = &hs[i];
        if (h->type == GM_HUB_MCP23017) {
            if (s->cfg_dirty) {
                mcp_write16(h->i2c_addr, MCP_IODIRA, ~s->dir_out & 0xFFFFu);
                mcp_write16(h->i2c_addr, MCP_GPPUA, s->pullup & 0xFFFFu);
                s->cfg_dirty = 0;
                s->never = 1;
            }
            if (s->out != s->written || s->never) {
                mcp_write16(h->i2c_addr, MCP_OLATA, s->out);
                s->written = s->out;
                s->never = 0;
            }
        } else if (h->type == GM_HUB_PCF8574) {
            /* Inputs must stay written high. */
            uint32_t v = (s->out | ~s->dir_out) & 0xFFu;
            if (v != s->written || s->never) {
                uint8_t b = (uint8_t)v;
                hal_i2c_write(h->i2c_addr, &b, 1);
                s->written = v;
                s->never = 0;
            }
        }
    }
}
