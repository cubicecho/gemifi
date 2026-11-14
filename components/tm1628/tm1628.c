/*
 * tm1628.c: the tm1628 hub type, TM1628-class LED drivers (TM1628, TM1638,
 * TM1620 ...) bit-banged from the main loop (components/tm1628/README.md).
 * A hub type on the hub base (components/hub/gm_hub.h): its pins are bits
 * in the hub RAM, and send writes what changed. Its config (.tm) and state
 * (.u.tm) stay in the core (gm_device.h, gm_hub.h): the fade (gm_vpin.h
 * vpin_fade*, core/src/vpin.c) sets the chip's brightness from them.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_device_config.h"
#include "gm_hal.h"
#include "gm_hub.h"

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
    const gm_pin_t p[3] = { h->tm.clk, h->tm.dio, h->tm.stb };
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
static uint32_t tm1628_send(const gm_hub_t *h, gm_hub_state_t *s, uint32_t budget)
{
    if (!s->u.tm.dirty)
        return budget;
    uint32_t n = h->tm.ram_bytes;
    if (s->ram + n > GM_HUB_RAM)
        n = GM_HUB_RAM - s->ram;
    uint8_t *ram = &gm_hub_ram[s->ram], *sent = &gm_hub_ram_sent[s->ram];
    tm_io_t bus;
    const tm_io_t *io = &bus;
    tm_io_init(&bus, h);
    uint32_t ctl = s->u.tm.ctl ? s->u.tm.ctl : 0x88u | (h->tm.brightness & 7u);
    if (!s->never && ctl != s->u.tm.ctl_sent) {    /* a new brightness (fade:) */
        tm_cmd(&bus, ctl);
        s->u.tm.ctl_sent = (uint8_t)ctl;
        budget--;
    }
    uint32_t k = 0, open = 0, left = budget;
    if (s->never) {                         /* all of it now, whatever the budget */
        if (h->tm.grids >= 4u && h->tm.grids <= 7u)
            tm_cmd(io, h->tm.grids - 4u);
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

static void tm1628_init(const gm_hub_t *h, gm_hub_state_t *s)
{
    /* Idle: STB, CLK and DIO high. RAM all off, written on the first send
     * (s->never: the whole RAM). */
    gm_hub_mcu_out(h->tm.stb, 1);
    gm_hub_mcu_out(h->tm.clk, 1);
    gm_hub_mcu_out(h->tm.dio, 1);
    s->ram = gm_hub_ram_alloc(h->tm.ram_bytes);
    s->u.tm.dirty = 1;
    for (uint32_t k = 0; k < h->tm.ram_bytes && s->ram + k < GM_HUB_RAM; k++)
        gm_hub_ram[s->ram + k] = 0;
}

/* Pin num = RAM bit, byte * 8 + bit. */
static void tm1628_write(const gm_hub_t *h, gm_hub_state_t *s, uint32_t num, int level)
{
    uint32_t k = num >> 3u;
    if (k >= h->tm.ram_bytes || s->ram + k >= GM_HUB_RAM)
        return;
    uint8_t *b = &gm_hub_ram[s->ram + k], m = (uint8_t)(1u << (num & 7u));
    uint8_t v = level ? (uint8_t)(*b | m) : (uint8_t)(*b & ~m);
    if (v != *b) {
        *b = v;
        s->u.tm.dirty = 1;
    }
}

const gm_hub_ops_t gm_tm1628_ops = {
    .flags = GM_HUBF_NO_READ, .init = tm1628_init, .write = tm1628_write, .send = tm1628_send,
};
