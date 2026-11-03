/*
 * vpin.c: virtual pins. See gm_vpin.h.
 *
 * Hub control pins (data, clock, latch, select, common, enable) must be MCU
 * pins (hub 0). /E of a mux and /OE of a 595 are driven low (enabled).
 * TM1628 hubs keep their display RAM in tm_ram (GM_TM1628_RAM bytes, all
 * TM1628 hubs back to back), since a chip has more than 32 bits.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_vpin.h"
#include "gm_hal.h"
#include "gm_device_config.h"

#define N_HUBS (GM_N_HUBS > 0 ? GM_N_HUBS : 1)
#ifndef GM_TM1628_RAM
#define GM_TM1628_RAM 0             /* no TM1628 hub */
#endif

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
    uint16_t tm_off;      /* TM1628: its bytes in tm_ram                  */
} hub_state_t;

static hub_state_t hs[N_HUBS];
#if GM_TM1628_RAM > 0
static uint8_t tm_ram[GM_TM1628_RAM];
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

void vpin_init(void)
{
#if GM_TM1628_RAM > 0
    uint32_t tm_off = 0;
#endif
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
            s->tm_dirty = 1;
            for (uint32_t k = 0; k < h->ram_bytes && tm_off < GM_TM1628_RAM; k++)
                tm_ram[tm_off++] = 0;
            break;
#endif
        default:
            break;
        }
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
    if (!h || h->type == GM_HUB_TM1628)
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
    if (h->type == GM_HUB_TM1628) {
        vpin_write(p, 0);
        return;
    }
    hs[p.hub - 1].dir_out |= 1u << p.num;
    hs[p.hub - 1].cfg_dirty = 1;
    vpin_write(p, 0);
}

static void mux_select(const gm_hub_t *h, hub_state_t *s, uint32_t ch)
{
    if (s->channel == (int8_t)ch)
        return;
    for (uint32_t k = 0; k < h->n_sel && k < 4; k++)
        if (h->sel[k].hub == 0)
            hal_pin_write(h->sel[k].num, (ch >> k) & 1u);
    s->channel = (int8_t)ch;
    hal_delay_us(h->settle_us ? h->settle_us : 2);
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
    if (h->type == GM_HUB_TM1628)
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
#if GM_TM1628_RAM > 0
    if (h->type == GM_HUB_TM1628) {
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

int32_t vpin_analog(gm_pin_t p)
{
    int32_t v;
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

void vpin_flush_tm1628(void)
{
    for (uint32_t i = 0; i < gm_device.n_hubs; i++) {
        const gm_hub_t *h = &gm_device.hubs[i];
        hub_state_t *s = &hs[i];
        if (h->type != GM_HUB_TM1628 || !s->tm_dirty)
            continue;
        s->tm_dirty = 0;
        uint32_t n = h->ram_bytes;
        if (s->tm_off + n > GM_TM1628_RAM)
            n = GM_TM1628_RAM - s->tm_off;
        /* Mode (if set), data write with auto-increment, address 0 and
         * the whole RAM, then display on at the brightness. */
        if (h->grids >= 4u && h->grids <= 7u)
            tm_cmd(h, h->grids - 4u);
        tm_cmd(h, 0x40u);
        tm_start(h);
        tm_byte(h, 0xC0u);
        for (uint32_t k = 0; k < n; k++)
            tm_byte(h, tm_ram[s->tm_off + k]);
        tm_end(h);
        tm_cmd(h, 0x88u | (h->brightness & 7u));
    }
}
#else
void vpin_flush_tm1628(void)
{
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
