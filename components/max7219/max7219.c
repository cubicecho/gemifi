/*
 * max7219.c: the max7219 hub type, a chain of MAX7219 / MAX7221 LED drivers
 * bit-banged from the main loop (components/max7219/README.md). A hub type
 * on the hub base (components/hub/gm_hub.h): its pins are bits in the hub
 * RAM (gm_hub_ledram_*), and send writes what changed.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_device_config.h"
#include "gm_hal.h"
#include "gm_hub.h"

/* A bus phase: n hal_cycles() (hal_ns_to_cycles), or 1 us without a
 * cycle counter (n = 0). */
static void bus_wait(uint32_t n)
{
    if (!n) {
        hal_delay_us(1);
        return;
    }
    uint32_t t = hal_cycles();
    while (hal_cycles() - t < n) {
    }
}

/* MAX7219: a 16-bit word per chip, MSB first, DIN taken on the CLK rising
 * edge; the chain's words are taken on CS rising, the last chip's sent
 * first. Datasheet: CLK <= 10 MHz, CLK high / low >= 50 ns, DIN setup 25 ns,
 * CS high >= 50 ns. Each phase waits MX_PHASE_NS on hal_cycles() where
 * there is a counter (a word ~8 us at 72 MHz; a frame, one register in
 * every chip, counts 2 a chip against the budget); without one, 1 us (a
 * word ~35 us: the ESP and NUC builds). */
#ifndef MX_PHASE_NS
#define MX_PHASE_NS 100u
#endif
#define MX_DECODE 0x09u
#define MX_INTENSITY 0x0Au
#define MX_SCAN_LIMIT 0x0Bu
#define MX_SHUTDOWN 0x0Cu
#define MX_TEST 0x0Fu

/* RAM byte (segments A..G = bits 0..6, DP bit 7) -> digit register (DP D7,
 * A D6 ... G D0). */
static uint32_t mx_bits(uint32_t v)
{
    uint32_t r = v & 0x80u;
    for (uint32_t s = 0; s < 7u; s++)
        if ((v >> s) & 1u)
            r |= 0x40u >> s;
    return r;
}

static void mx_word(const gm_hub_t *h, uint32_t w, uint32_t ph)
{
    for (uint32_t b = 16; b-- > 0;) {
        hal_pin_write(h->mx.din.num, (int)((w >> b) & 1u));
        bus_wait(ph);
        hal_pin_write(h->mx.clk.num, 1);
        bus_wait(ph);
        hal_pin_write(h->mx.clk.num, 0);
    }
}

/* Register reg of every chip: digit register reg's RAM byte per chip
 * (ram), or v in each (ram 0). */
static void mx_frame(const gm_hub_t *h, uint32_t reg, const uint8_t *ram, uint32_t v, uint32_t ph)
{
    hal_pin_write(h->mx.cs.num, 0);
    for (uint32_t c = h->mx.chips; c-- > 0;)
        mx_word(h, reg << 8 | (ram ? mx_bits(ram[c * 8u + reg - 1u]) : v), ph);
    bus_wait(ph);
    hal_pin_write(h->mx.cs.num, 1);
    bus_wait(ph);
}

/* The first write sets the chips up (test off, no decode, scan limit,
 * intensity), writes every digit, then turns them on; after that, each
 * digit that changed in any chip is one frame. At least one frame a call,
 * then as many as the budget has room for. */
static uint32_t max7219_send(const gm_hub_t *h, gm_hub_state_t *s, uint32_t budget)
{
    uint32_t n = h->mx.chips * 8u, cost = 2u * h->mx.chips;
    if (!s->u.seg.dirty || s->ram + n > GM_HUB_RAM)
        return budget;
    uint8_t *ram = &gm_hub_ram[s->ram], *sent = &gm_hub_ram_sent[s->ram];
    uint32_t ph = hal_ns_to_cycles(MX_PHASE_NS);
    if (s->never) {
        mx_frame(h, MX_TEST, 0, 0, ph);
        mx_frame(h, MX_DECODE, 0, 0, ph);
        mx_frame(h, MX_SCAN_LIMIT, 0, (h->mx.scan_limit - 1u) & 7u, ph);
        mx_frame(h, MX_INTENSITY, 0, h->mx.intensity & 15u, ph);
        for (uint32_t d = 1; d <= 8u; d++)
            mx_frame(h, d, ram, 0, ph);
        mx_frame(h, MX_SHUTDOWN, 0, 1, ph);
        for (uint32_t k = 0; k < n; k++)
            sent[k] = ram[k];
        s->never = 0;
        s->u.seg.dirty = 0;
        return budget > 13u * cost ? budget - 13u * cost : 0u;
    }
    uint32_t d = 0, left = 0;
    for (; d < 8u; d++) {
        uint32_t c = 0;
        while (c < h->mx.chips && ram[c * 8u + d] == sent[c * 8u + d])
            c++;
        if (c == h->mx.chips)
            continue;
        if (!budget) {
            left = 1;
            break;
        }
        mx_frame(h, d + 1u, ram, 0, ph);
        for (c = 0; c < h->mx.chips; c++)
            sent[c * 8u + d] = ram[c * 8u + d];
        budget = budget > cost ? budget - cost : 0u;
    }
    s->u.seg.dirty = (uint8_t)left;
    return budget;
}

static void max7219_init(const gm_hub_t *h, gm_hub_state_t *s)
{
    gm_hub_mcu_out(h->mx.cs, 1);        /* idle: CS high, CLK low */
    gm_hub_mcu_out(h->mx.clk, 0);
    gm_hub_mcu_out(h->mx.din, 0);
    gm_hub_ledram_init(s, h->mx.chips * 8u);
}

static void max7219_write(const gm_hub_t *h, gm_hub_state_t *s, uint32_t num, int level)
{
    gm_hub_ledram_write(s, h->mx.chips * 8u, num, level);
}

const gm_hub_ops_t gm_max7219_ops = {
    .flags = GM_HUBF_NO_READ, .init = max7219_init, .write = max7219_write, .send = max7219_send,
};
