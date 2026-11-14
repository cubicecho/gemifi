/*
 * tm1637.c: the tm1637 LED driver platform, a TM1637 2-wire LED driver
 * bit-banged from the main loop (components/tm1637/README.md). A hub type
 * on the led_driver base (components/led_driver): its pins are bits in the
 * hub RAM (gm_led_driver_*), and send writes what changed.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_device_config.h"
#include "gm_hal.h"
#include "gm_hub.h"
#include "led_driver.h"

/* TM1637: 2-wire, start (DIO falls while CLK is high), bytes LSB first
 * (DIO set while CLK is low, taken on its rising edge), each followed by
 * the chip's ACK clock, stop (DIO rises while CLK is high). The ACK isn't
 * read: DIO is driven low through it, the level the chip pulls it to, so a
 * push-pull DIO never fights the chip. Each phase is TM37_HALF_US (a bit
 * ~2 x 3 us plus the pin writes, ~60 us a byte): the chip's clock is slow,
 * and boards often have capacitors on both lines. A bus byte costs
 * TM37_COST of the budget (about its time in TM1628 bytes). */
#ifndef TM37_HALF_US
#define TM37_HALF_US 3u
#endif
#define TM37_COST 5u

static void t37_start(const gm_hub_t *h)
{
    hal_pin_write(h->tm37.dio.num, 0);
    hal_delay_us(TM37_HALF_US);
    hal_pin_write(h->tm37.clk.num, 0);
    hal_delay_us(TM37_HALF_US);
}

static void t37_stop(const gm_hub_t *h)
{
    hal_pin_write(h->tm37.dio.num, 0);
    hal_delay_us(TM37_HALF_US);
    hal_pin_write(h->tm37.clk.num, 1);
    hal_delay_us(TM37_HALF_US);
    hal_pin_write(h->tm37.dio.num, 1);
    hal_delay_us(TM37_HALF_US);
}

static void t37_byte(const gm_hub_t *h, uint32_t v)
{
    for (uint32_t b = 0; b < 9u; b++, v >>= 1) {     /* 8 bits, then the ACK clock with DIO low */
        hal_pin_write(h->tm37.dio.num, (int)(b < 8u ? v & 1u : 0u));
        hal_delay_us(TM37_HALF_US);
        hal_pin_write(h->tm37.clk.num, 1);
        hal_delay_us(TM37_HALF_US);
        hal_pin_write(h->tm37.clk.num, 0);
    }
}

static void t37_cmd(const gm_hub_t *h, uint32_t cmd)
{
    t37_start(h);
    t37_byte(h, cmd);
    t37_stop(h);
}

/* The first write: 0x40 (write, auto-increment), every grid from C0, then
 * 0x88 | brightness (on). After that, the first run of changed grids: 0x40,
 * then C0 | its first address and its bytes, as many as the budget has
 * room for (at least one); the rest on the next calls. */
static uint32_t tm1637_send(const gm_hub_t *h, gm_hub_state_t *s, uint32_t budget)
{
    uint32_t n = h->tm37.grids;
    if (!s->u.led_driver.dirty || s->ram + n > GM_HUB_RAM)
        return budget;
    uint8_t *ram = &gm_hub_ram[s->ram], *sent = &gm_hub_ram_sent[s->ram];
    uint32_t k = 0, all = s->never;
    if (!all)
        while (k < n && ram[k] == sent[k])
            k++;
    if (k < n) {
        t37_cmd(h, 0x40u);
        t37_start(h);
        t37_byte(h, 0xC0u | k);
        uint32_t used = 2u * TM37_COST;
        do {
            t37_byte(h, ram[k]);
            sent[k] = ram[k];
            k++;
            used += TM37_COST;
        } while (k < n && (all || (ram[k] != sent[k] && used + TM37_COST <= budget)));
        t37_stop(h);
        budget = budget > used ? budget - used : 0u;
    }
    if (all) {
        t37_cmd(h, 0x88u | (h->tm37.brightness & 7u));
        s->never = 0;
    }
    while (k < n && ram[k] == sent[k])
        k++;
    s->u.led_driver.dirty = k < n;
    return budget;
}

static void tm1637_init(const gm_hub_t *h, gm_hub_state_t *s)
{
    gm_hub_mcu_out(h->tm37.clk, 1);     /* idle: both high */
    gm_hub_mcu_out(h->tm37.dio, 1);
    gm_led_driver_init(s, h->tm37.grids);
}

static void tm1637_write(const gm_hub_t *h, gm_hub_state_t *s, uint32_t num, int level)
{
    gm_led_driver_write(s, h->tm37.grids, num, level);
}

const gm_hub_ops_t gm_tm1637_ops = {
    .flags = GM_HUBF_NO_READ, .init = tm1637_init, .write = tm1637_write, .send = tm1637_send,
};
