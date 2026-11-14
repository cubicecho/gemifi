/*
 * vpin.c: virtual pins. See gm_vpin.h.
 *
 * Hubs are drivers behind one interface (gm_hub.h): each hub type has a
 * gm_hub_ops_t, in hub_ops[] at the bottom, one section per type above it.
 * Hub control pins (data, clock, latch, select, common, enable) must be MCU
 * pins (hub 0). /E of a mux and /OE of a 595 are driven low (enabled).
 * TM1628, MAX7219, TM1637, HT16K33, MAUDIO_PADS and WS2812 hubs keep their
 * pins (an SSD1306 its framebuffer) in the hub RAM (gm_hub_ram, GM_HUB_RAM
 * bytes, each hub's back to back from s->ram), since a chip has more than
 * 32 bits: an LED driver's display RAM (a MAX7219's digit registers); the
 * LEDs of the pad LED board and of an LED strip a byte per colour (pin = LED * 3 + colour, LED * 4 on an RGBW
 * strip: a pin on is 255, off 0; vpin_rgb sets any colour). gm_hub_ram_sent[] has
 * what the chip, board or strip was sent.
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
#if !defined(GM_HUB_RAM) && defined(GM_TM1628_RAM)
#define GM_HUB_RAM GM_TM1628_RAM    /* a config generated before the rename */
#endif
#ifndef GM_HUB_RAM
#define GM_HUB_RAM 0                /* no hub keeps its pins in the hub RAM */
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
    for (uint32_t k = 0; k < h->dec.n_addr + h->dec.n_en && k < 7u; k++) {
        gm_pin_t p = k < h->dec.n_addr ? h->dec.addr[k] : h->dec.en[k - h->dec.n_addr];
        uint32_t b = 0;
        volatile uint32_t *r = p.hub ? 0 : hal_pin_bsrr(p.num, &b);
        if (!r || (reg && r != reg))
            return;
        reg = r;
        if (k < h->dec.n_addr)
            abit[k] = b;
        else
            ebit[k - h->dec.n_addr] = b;
    }
    if (!reg)
        return;
    uint32_t slot = dec_used++, per = 1u << h->dec.n_addr;
    for (uint32_t n = 0; n < DEC_OUT; n++) {
        uint32_t w = 0, g = n / per, lv = g < 4u ? h->dec.group_lv[g] : 0;
        for (uint32_t k = 0; k < h->dec.n_addr; k++)
            w |= ((n >> k) & 1u) ? 1u << abit[k] : 1u << (abit[k] + 16u);
        for (uint32_t k = 0; k < h->dec.n_en; k++)
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
    for (uint32_t c = 0; c < h->sm.n_cols; c++) {
        uint32_t mask = 0;
        const volatile uint32_t *r = h->sm.cols[c].hub ? 0 : hal_pin_in_reg(h->sm.cols[c].num, &mask);
        if (!r || !mask || (mask & (mask - 1u)))
            return;
        uint32_t bit = (uint32_t)__builtin_ctz(mask);
        if (c == 0) {
            reg = r;
            sm_shift[i] = (uint8_t)bit;
        } else if (r != reg || bit != sm_shift[i] + c) {
            return;
        }
        if (h->sm.cols[c].flags & GM_PIN_INVERT)
            inv |= 1u << c;
    }
    if (h->sm.n_cols == 0 || sm_shift[i] + h->sm.n_cols > 32u)
        return;
    sm_inv[i] = inv;
    sm_reg[i] = reg;
}
#if GM_HUB_RAM > 0
uint8_t gm_hub_ram[GM_HUB_RAM];
uint8_t gm_hub_ram_sent[GM_HUB_RAM];   /* what the chip / LED board has (its bytes of gm_hub_ram) */
static uint32_t ram_next;                 /* vpin_init: the next free byte */

/* n bytes of the hub RAM for the hub being initialised (as many as are left). */
uint16_t gm_hub_ram_alloc(uint32_t n)
{
    uint32_t at = ram_next;
    ram_next = at + n < GM_HUB_RAM ? at + n : GM_HUB_RAM;
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

void gm_hub_mcu_out(gm_pin_t p, int level)
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

static void mux_masks(uint32_t i, const gm_hub_t *h);   /* the multiplexers', below */

/* The tables of the scan's fast paths, written out in this file (vpin_read
 * on a mux channel, vpin_write on a decoder row, the switch-matrix scan),
 * for hub i once its driver's init has run. */
static void fast_init(uint32_t i, const gm_hub_t *h)
{
    if (USES(GM_HUB_MUX) && h->type == GM_HUB_MUX)
        mux_masks(i, h);
}

/* Hubs refreshed / flushed from the scan tick, in hub order: the tick goes
 * through these lists, not every hub. */
static uint8_t scan_in[N_HUBS], scan_out[N_HUBS];
static uint8_t n_scan_in, n_scan_out;

void vpin_init(void)
{
#if GM_HUB_RAM > 0
    ram_next = 0;
#endif
    n_fed = 0;
    fed_on = 0;
    n_scan_in = n_scan_out = 0;
    dec_used = 0;                   /* gm_buttons_now, then gm_init: each init fills the tables again */
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
        fast_init(i, h);
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

static void decoder_select(const gm_hub_t *h, uint32_t i, uint32_t n);

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
        if (h->mux.common.hub != 0)
            return 0;
        gm_hub_mux_select(h, p.hub - 1u, p.num);
        return hal_pin_read(h->mux.common.num) ^ inv;
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
    if (h->type == GM_HUB_DECODER) {
        if (active)
            decoder_select(h, p.hub - 1u, p.num);
        return;
    }
    hub_state_t *s = &hs[p.hub - 1];
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

/* ---- XY pads: a new touch (the driver: components/xy_pad) ------------- */

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

/* ---- 74HC4051 / 4067 multiplexers: the scan's path ----------------------
 * The driver is components/cd74hc4067. Muxes may share select lines
 * (allow_other_uses): switching one switches the others. One with the same
 * lines is now on the same channel; one that shares only some must switch
 * again. Which muxes share select lines with which is worked out once
 * (mux_masks, from vpin_init): bit j of mux_same[i] = hub j has
 * exactly hub i's lines (so it is now on the same channel), of mux_some[i] =
 * it shares only some (so it must switch again). Masks cover hubs 0-31; a
 * device with more falls back to the loop. Read digitally by vpin_read
 * itself (the scan's path), as analog by the analog op or the hardware
 * analog scan. */
static uint32_t mux_same[N_HUBS], mux_some[N_HUBS];
/* a constant with LTO: the loop below goes when there are <= 32 hubs */
#define MUX_MASKS (gm_device.n_hubs <= 32u)

static void mux_pair(const gm_hub_t *h, const gm_hub_t *o, int *any, int *same)
{
    uint32_t n = 0;
    *any = 0;
    for (uint32_t k = 0; k < o->mux.n_sel && k < 4; k++)
        for (uint32_t j = 0; j < h->mux.n_sel && j < 4; j++)
            if (o->mux.sel[k].hub == 0 && h->mux.sel[j].hub == 0 && o->mux.sel[k].num == h->mux.sel[j].num) {
                *any = 1;
                n += j == k;
            }
    *same = n == o->mux.n_sel && o->mux.n_sel == h->mux.n_sel;
}

/* Hub i's masks (vpin_init, after its init). */
static void mux_masks(uint32_t i, const gm_hub_t *h)
{
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

static void mux_shared(const gm_hub_t *h, uint32_t i, uint32_t ch)
{
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

/* Hub i (h) to channel ch (gm_hub.h: the mux's analog op uses it too). */
void gm_hub_mux_select(const gm_hub_t *h, uint32_t i, uint32_t ch)
{
    if (hs[i].channel == (int8_t)ch)
        return;
    for (uint32_t k = 0; k < h->mux.n_sel && k < 4; k++)
        if (h->mux.sel[k].hub == 0)
            hal_pin_write(h->mux.sel[k].num, (ch >> k) & 1u);
    hs[i].channel = (int8_t)ch;
    mux_shared(h, i, ch);
    hal_delay_us(h->mux.settle_us ? h->mux.settle_us : 2);
}

/* ---- decoders (74HC138 style) ------------------------------------------
 * Written by vpin_write itself (the scan's path): "on" selects the output,
 * "off" does nothing (the next row's select replaces it). */

static void decoder_init(const gm_hub_t *h, hub_state_t *s)
{
    for (uint32_t k = 0; k < h->dec.n_addr && k < 4; k++)
        gm_hub_mcu_out(h->dec.addr[k], 0);
    for (uint32_t k = 0; k < h->dec.n_en && k < 3; k++)
        gm_hub_mcu_out(h->dec.en[k], 0);
    dec_fast_init((uint32_t)(s - hs), h);
}

/* Decoder: address lines = n's low bits, enable lines = its group's levels.
 * Written only when the selection changes. */
static void decoder_select(const gm_hub_t *h, uint32_t i, uint32_t n)
{
    /* One store: always written, so a key scan that selects rows itself
     * (vpin_select_word) never leaves this out of date. */
    hub_state_t *s = &hs[i];
    int8_t slot = dec_slot[i];
    if (slot >= 0 && n < DEC_OUT) {
        *dec_reg[slot] = dec_word[slot][n];
        return;
    }
    if (s->channel == (int8_t)n)
        return;
    uint32_t g = n >> h->dec.n_addr;
    for (uint32_t k = 0; k < h->dec.n_addr && k < 4; k++)
        if (h->dec.addr[k].hub == 0)
            hal_pin_write(h->dec.addr[k].num, (n >> k) & 1u);
    uint32_t lv = g < 4 ? h->dec.group_lv[g] : 0;
    for (uint32_t k = 0; k < h->dec.n_en && k < 3; k++)
        if (h->dec.en[k].hub == 0)
            hal_pin_write(h->dec.en[k].num, (lv >> k) & 1u);
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
    if (!h || h->type != GM_HUB_MUX || h->mux.common.hub != 0 || h->mux.n_sel == 0 || h->mux.n_sel > 4)
        return 0;
    if (!*group) {
        uint32_t steps = 1u << h->mux.n_sel;
        if (steps > ADC_STEPS)
            return 0;
        volatile uint32_t *reg = 0;
        uint32_t bit[4];
        for (uint32_t k = 0; k < h->mux.n_sel; k++) {
            volatile uint32_t *r = h->mux.sel[k].hub ? 0 : hal_pin_bsrr(h->mux.sel[k].num, &bit[k]);
            if (!r || (reg && r != reg))
                return 0;
            reg = r;
        }
        for (uint32_t c = 0; c < steps; c++) {
            uint32_t w = 0;
            for (uint32_t k = 0; k < h->mux.n_sel; k++)
                w |= ((c >> k) & 1u) ? 1u << bit[k] : 1u << (bit[k] + 16u);
            aplan.step_word[c] = w;
        }
        aplan.sel_reg = reg;
        aplan.n_steps = steps;
        *group = h;
    } else if (h != *group) {
        const gm_hub_t *g = *group;
        if (h->mux.n_sel != g->mux.n_sel)
            return 0;
        for (uint32_t k = 0; k < h->mux.n_sel; k++)
            if (h->mux.sel[k].hub != 0 || h->mux.sel[k].num != g->mux.sel[k].num)
                return 0;
    }
    uint32_t ns = (h->mux.settle_us ? h->mux.settle_us : 2u) * 1000u;   /* as gm_hub_mux_select waits */
    if (ns > aplan.settle_ns)
        aplan.settle_ns = ns;
    return adc_add_pin(h->mux.common.num) >= 0;
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
        *k = adc_pin_index(h->mux.common.num);
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
    for (uint32_t c = 0; c < h->sm.n_cols; c++)
        vpin_input(h->sm.cols[c]);
    for (uint32_t r = 0; r < h->sm.n_rows; r++)
        vpin_output(h->sm.rows[r]);
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
    return hub_of(p) && (hs[p.hub - 1].flags & GM_HUBF_SELECT);     /* (after vpin_init) */
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
        uint32_t all = h->sm.n_cols >= 32u ? 0xFFFFFFFFu : (1u << h->sm.n_cols) - 1u;
        /* rows_per_scan rows per call, in turn (the cursor lives in .channel,
         * unused by switch matrices); the other rows keep their last reading. */
        uint32_t n = h->sm.rows_per_scan && h->sm.rows_per_scan < h->sm.n_rows ? h->sm.rows_per_scan : h->sm.n_rows;
        uint32_t r = (s->channel >= 0 && (uint32_t)s->channel < h->sm.n_rows) ? (uint32_t)s->channel : 0;
        for (uint32_t k = 0; k < n; k++, r = r + 1u < h->sm.n_rows ? r + 1u : 0) {
            uint32_t bit = r * h->sm.n_cols;
            if (bit >= 32u)
                continue;
            vpin_write(h->sm.rows[r], 1);
            hal_delay_us(h->sm.settle_us ? h->sm.settle_us : 1);
            uint32_t row = 0;
            if (sm_reg[i]) {
                row = ((*sm_reg[i] >> sm_shift[i]) & all) ^ sm_inv[i];
            } else {
                for (uint32_t c = 0; c < h->sm.n_cols && bit + c < 32u; c++)
                    if (vpin_read(h->sm.cols[c]))
                        row |= 1u << c;
            }
            v = (v & ~(all << bit)) | (row << bit);
            if (!vpin_is_decoder(h->sm.rows[r]))
                vpin_write(h->sm.rows[r], 0);
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
        uint32_t all = h->sm.n_cols >= 32u ? 0xFFFFFFFFu : (1u << h->sm.n_cols) - 1u;
        mask |= all << sm_shift[i];
        for (uint32_t r = 0; r < h->sm.n_rows; r++) {
            if (r * h->sm.n_cols >= 32u)
                break;                  /* beyond the 32-bit cache, as vpin_scan_matrices */
            volatile uint32_t *reg = 0;
            uint32_t word = 0, k;
            if (!vpin_select_word(h->sm.rows[r], &reg, &word) || (sreg && reg != sreg))
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
        uint32_t all = h->sm.n_cols >= 32u ? 0xFFFFFFFFu : (1u << h->sm.n_cols) - 1u;
        uint32_t bit = fed_row[f] * h->sm.n_cols;
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

/* ---- TM1628 fade (gm_vpin.h vpin_fade*; driver: components/tm1628) ---- */

#if GM_HUB_RAM > 0
/* For the bit-banged LED drivers' bus timing (components/tm1628, max7219). */
__attribute__((weak)) uint32_t hal_ns_to_cycles(uint32_t ns)
{
    (void)ns;
    return 0;
}

#if GM_TM_FADE
/* How bright each step looks, 0..255: the chip's pulse widths (1, 2, 4,
 * 10, 11, 12, 13, 14 sixteenths) through gamma 2.2. */
static const uint8_t tm_seen[8] = { 72, 99, 136, 206, 215, 224, 232, 240 };

int vpin_fades(gm_pin_t p)
{
    const gm_hub_t *h = p.hub ? hub_of(p) : 0;
    return h && h->type == GM_HUB_TM1628 && h->tm.fade;
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
    uint32_t top = h->tm.brightness & 7u, want = v * tm_seen[top] / 255u, best = 0;
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
        if (!h->tm.fade || !peak)
            continue;
        uint32_t ctl = tm_fade_ctl(h, peak - 1u);
        if (ctl != (s->u.tm.ctl ? s->u.tm.ctl : (0x88u | (h->tm.brightness & 7u)))) {
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

/* ---- full-colour output (gm_hub.h) ------------------------------------ */

/* What goes on the wire for colour c (0 red, 1 green, 2 blue, 3 white) at
 * value v: through the hub's curve (gamma:), then scaled to its white
 * (white:, 0 = 255). A value above 0 stays above 0. Full-colour hubs
 * (components/maudio_pad_leds, ws2812) share it (gm_hub.h). */
uint8_t gm_hub_colour_out(const uint8_t *curve, const uint8_t *white, uint32_t c, uint32_t v)
{
    if (curve)
        v = curve[v];
    uint32_t w = white[c] ? white[c] : 255u;
    if (w == 255u || !v)
        return (uint8_t)v;
    v = (v * w + 127u) / 255u;
    return (uint8_t)(v ? v : 1u);
}
/* ---- bit pins in the hub RAM (the LED drivers', gm_hub.h) ------------- */

/* The pins of an LED driver with a bit per pin in the hub RAM (MAX7219,
 * TM1637, HT16K33: components/max7219, tm1637, ht16k33). Pin num = RAM
 * bit, byte * 8 + bit, of `bytes` bytes; marks the chip dirty if it
 * changed. */
void gm_hub_ledram_write(gm_hub_state_t *s, uint32_t bytes, uint32_t num, int level)
{
    uint32_t k = num >> 3u;
    if (k >= bytes || s->ram + k >= GM_HUB_RAM)
        return;
    uint8_t *b = &gm_hub_ram[s->ram + k], m = (uint8_t)(1u << (num & 7u));
    uint8_t v = level ? (uint8_t)(*b | m) : (uint8_t)(*b & ~m);
    if (v != *b) {
        *b = v;
        s->u.seg.dirty = 1;
    }
}

/* n bytes of the hub RAM, all off, written whole on the first send. */
void gm_hub_ledram_init(gm_hub_state_t *s, uint32_t n)
{
    s->ram = gm_hub_ram_alloc(n);
    s->u.seg.dirty = 1;
    for (uint32_t k = 0; k < n && s->ram + k < GM_HUB_RAM; k++)
        gm_hub_ram[s->ram + k] = 0;
}

/* ---- SSD1306 / SH1106 OLEDs (main loop) -------------------------------- */

/* SSD1306 / SH1106 (I2C OLED): a framebuffer in the hub RAM, byte = page *
 * width + x, bit 0 at the top of the page; text displays draw into it
 * (vpin_oled_draw) and s->u.oled.pages marks the pages changed. The first
 * write: the set-up commands (display off), every page, then display on.
 * After that, runs of changed bytes, page by page: one write each, its page
 * and column (each command after a 0x80 control byte), then 0x40 and up to
 * OLED_RUN data bytes (a write fits the ESP Wire buffer, 128 bytes). A run
 * is ~24 bus bytes (~0.55 ms at 400 kHz): at least one goes out a pass,
 * more while the budget lasts (a bus byte counts OLED_COST, as an
 * HT16K33's); vpin_send_all sends everything. The SH1106's RAM is 132
 * columns, the panel's 128 from column 2. The writes aren't checked, as
 * the other I2C hubs' aren't. */
#define OLED_RUN 16u
#define OLED_HEAD 7u                /* page, column low, column high (3 x 2), 0x40 */
#define OLED_COST 2u

static uint32_t oled_bytes(const gm_hub_t *h)
{
    return (uint32_t)h->oled.width * (h->oled.height / 8u);
}

static void oled_setup(const gm_hub_t *h)
{
    const gm_hub_ssd1306_t *o = &h->oled;
    int sh = (o->flags & GM_OLED_SH1106) != 0, rot = (o->flags & GM_OLED_ROTATE) != 0;
    uint8_t b[32];
    uint32_t n = 0;
    b[n++] = 0x00;                          /* control: commands follow */
    b[n++] = 0xAE;                          /* display off */
    b[n++] = 0xD5; b[n++] = 0x80;           /* clock divide, oscillator */
    b[n++] = 0xA8; b[n++] = (uint8_t)(o->height - 1u);   /* multiplex ratio */
    b[n++] = 0xD3; b[n++] = 0x00;           /* display offset */
    b[n++] = 0x40;                          /* start line 0 */
    if (sh) {
        b[n++] = 0xAD; b[n++] = 0x8B;       /* SH1106: DC-DC on */
    } else {
        b[n++] = 0x8D; b[n++] = 0x14;       /* SSD1306: charge pump on */
        b[n++] = 0x20; b[n++] = 0x02;       /* page addressing (as the SH1106 has) */
    }
    b[n++] = rot ? 0xA0 : 0xA1;             /* segment remap */
    b[n++] = rot ? 0xC0 : 0xC8;             /* COM scan direction */
    b[n++] = 0xDA; b[n++] = o->height > 32u ? 0x12 : 0x02;   /* COM pins */
    b[n++] = 0x81; b[n++] = o->contrast;
    b[n++] = 0xD9; b[n++] = sh ? 0x22 : 0xF1;   /* pre-charge */
    b[n++] = 0xDB; b[n++] = sh ? 0x35 : 0x40;   /* VCOMH */
    b[n++] = 0xA4;                          /* show the RAM */
    b[n++] = 0xA6;                          /* not inverted */
    hal_i2c_write(o->addr, b, n);
}

/* n (<= OLED_RUN) framebuffer bytes from column x of page pg. */
static void oled_write(const gm_hub_t *h, uint32_t pg, uint32_t x, const uint8_t *d, uint32_t n)
{
    uint8_t b[OLED_HEAD + OLED_RUN];
    uint32_t col = x + ((h->oled.flags & GM_OLED_SH1106) ? 2u : 0u), k = 0;
    b[k++] = 0x80; b[k++] = (uint8_t)(0xB0u | pg);
    b[k++] = 0x80; b[k++] = (uint8_t)(col & 0x0Fu);
    b[k++] = 0x80; b[k++] = (uint8_t)(0x10u | (col >> 4));
    b[k++] = 0x40;                          /* data follows */
    for (uint32_t i = 0; i < n; i++)
        b[k++] = d[i];
    hal_i2c_write(h->oled.addr, b, k);
}

static uint32_t ssd1306_send(const gm_hub_t *h, hub_state_t *s, uint32_t budget)
{
    uint32_t w = h->oled.width, np = h->oled.height / 8u;
    if (!s->u.oled.pages || s->ram + oled_bytes(h) > GM_HUB_RAM)
        return budget;
    uint8_t *ram = &gm_hub_ram[s->ram], *sent = &gm_hub_ram_sent[s->ram];
    uint32_t all = s->never, used = 0;
    if (all)
        oled_setup(h);
    for (uint32_t pg = 0; pg < np; pg++) {
        if (!(s->u.oled.pages & (1u << pg)))
            continue;
        uint8_t *r = ram + pg * w, *t = sent + pg * w;
        uint32_t x = 0;
        for (;;) {
            if (!all)
                while (x < w && r[x] == t[x])
                    x++;
            if (x >= w) {
                s->u.oled.pages = (uint8_t)(s->u.oled.pages & ~(1u << pg));
                break;
            }
            if (!all && used && used >= budget)
                return 0;                   /* the rest on the next passes */
            uint32_t end = x + OLED_RUN < w ? x + OLED_RUN : w;
            if (!all)
                while (end > x + 1u && r[end - 1u] == t[end - 1u])
                    end--;
            oled_write(h, pg, x, r + x, end - x);
            for (uint32_t i = x; i < end; i++)
                t[i] = r[i];
            used += (OLED_HEAD + 1u + end - x) * OLED_COST;
            x = end;
        }
    }
    if (all) {
        uint8_t on[2] = { 0x00, 0xAF };     /* display on, the RAM now clear (or drawn) */
        hal_i2c_write(h->oled.addr, on, 2);
        s->never = 0;
    }
    return budget > used ? budget - used : 0u;
}

static void ssd1306_init(const gm_hub_t *h, hub_state_t *s)
{
    uint32_t n = oled_bytes(h);
    s->ram = gm_hub_ram_alloc(n);
    for (uint32_t k = 0; k < n && s->ram + k < GM_HUB_RAM; k++)
        gm_hub_ram[s->ram + k] = 0;
    s->u.oled.pages = (uint8_t)((1u << (h->oled.height / 8u)) - 1u);
}

void vpin_oled_draw(gm_pin_t at, const uint8_t *cols, uint32_t n, uint32_t scale)
{
    const gm_hub_t *h = hub_of(at);
    if (!h || h->type != GM_HUB_SSD1306)
        return;
    hub_state_t *s = &hs[at.hub - 1];
    uint32_t w = h->oled.width, np = h->oled.height / 8u;
    if (s->ram + oled_bytes(h) > GM_HUB_RAM)
        return;
    uint8_t *fb = &gm_hub_ram[s->ram];
    uint32_t page = at.num / w, x0 = at.num % w;
    scale = scale < 1u ? 1u : scale > 4u ? 4u : scale;
    uint32_t ones = (1u << scale) - 1u;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = 0;                     /* the column, each pixel scale tall */
        for (uint32_t b = 0; b < 8u; b++)
            if (cols[i] & (1u << b))
                v |= ones << (b * scale);
        for (uint32_t r = 0; r < scale; r++) {
            uint32_t x = x0 + i * scale + r;
            if (x >= w)
                return;
            for (uint32_t k = 0; k < scale && page + k < np; k++) {
                uint8_t *p = &fb[(page + k) * w + x], byte = (uint8_t)(v >> (8u * k));
                if (*p != byte) {
                    *p = byte;
                    s->u.oled.pages = (uint8_t)(s->u.oled.pages | (1u << (page + k)));
                }
            }
        }
    }
}
#else
void vpin_oled_draw(gm_pin_t at, const uint8_t *cols, uint32_t n, uint32_t scale)
{
    (void)at;
    (void)cols;
    (void)n;
    (void)scale;
}

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

/* ---- the I2C expanders' pins (gm_hub.h) ------------------------------- */

/* The I2C expanders' pins (MCP23017, PCF8574: components/mcp23017,
 * pcf8574): their directions and pull-ups in u.io, which flush writes. */
void gm_hub_i2c_init(const gm_hub_t *h, gm_hub_state_t *s)
{
    (void)h;
    s->u.io.cfg_dirty = 1;
}

void gm_hub_i2c_input(const gm_hub_t *h, gm_hub_state_t *s, gm_pin_t p)
{
    s->u.io.dir_out &= ~(1u << p.num);
    s->u.io.in_used |= 1u << p.num;
    if (p.flags & GM_PIN_PULLUP)
        s->u.io.pullup |= 1u << p.num;
    if (h->type == GM_HUB_PCF8574)
        s->out |= 1u << p.num;          /* quasi-bidirectional: input = high */
    s->u.io.cfg_dirty = 1;
}

void gm_hub_i2c_output(const gm_hub_t *h, gm_hub_state_t *s, gm_pin_t p)
{
    (void)h;
    s->u.io.dir_out |= 1u << p.num;
    s->u.io.cfg_dirty = 1;
}

/* ---- the table: one gm_hub_ops_t per hub type -------------------------- */

static const gm_hub_ops_t decoder_ops = {
    .flags = GM_HUBF_SELECT, .init = decoder_init,  /* writes: vpin_write */
};
static const gm_hub_ops_t swmatrix_ops = {
    .flags = GM_HUBF_IN_REG | GM_HUBF_MATRIX, .init = swmatrix_init,   /* scan: vpin_scan_matrices */
};
#if GM_HUB_RAM > 0
static const gm_hub_ops_t ssd1306_ops = {
    .flags = GM_HUBF_NO_READ, .init = ssd1306_init, .send = ssd1306_send,   /* drawn: vpin_oled_draw */
};
#endif
static const gm_hub_ops_t no_ops = { 0 };

#define HUB(t, ops) [t] = USES(t) ? &(ops) : 0
GM_HUB_OPS_EXTERNS          /* the hub types in components/ (gm_components.h) */

static const gm_hub_ops_t *const hub_ops[GM_HUB_N_TYPES] = {
    HUB(GM_HUB_DECODER, decoder_ops),
    HUB(GM_HUB_SWMATRIX, swmatrix_ops),
#if GM_HUB_RAM > 0
    HUB(GM_HUB_SSD1306, ssd1306_ops),
#endif
    GM_HUB_OPS
};

static const gm_hub_ops_t *hub_ops_of(uint32_t type)
{
    const gm_hub_ops_t *o = type < GM_HUB_N_TYPES ? hub_ops[type] : 0;
    return o ? o : &no_ops;
}
