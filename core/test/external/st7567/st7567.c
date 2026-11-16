/*
 * st7567.c: the st7567 display platform, an ST7567 128x64 LCD on the I2C
 * bus (an external component: core/README.md "External components"). The
 * first write sets the panel up and sends every page; after that, each
 * changed page whole (one write: its page and column, then 128 bytes).
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_hal.h"
#include "gm_screen.h"

#define ST_COST 2u

static void st_setup(const gm_screen_t *c)
{
    const gm_screen_st7567_t *o = &c->st7567;
    uint8_t b[] = { 0x00, 0xE2, 0xA2, 0xA0, 0xC8, 0x2F, 0x25, 0x81, (uint8_t)(o->contrast & 0x3Fu), 0x40, 0xAF };
    hal_i2c_write(o->bus, o->addr, b, sizeof b);
}

static uint32_t st7567_send(const gm_screen_t *c, gm_screen_state_t *s, const uint8_t *fb, uint8_t *sent,
                            uint32_t budget)
{
    uint32_t w = c->width, np = c->height / 8u, used = 0;
    uint8_t b[4 + 128];
    if (s->never)
        st_setup(c);
    for (uint32_t pg = 0; pg < np; pg++) {
        if (!(s->pages & (1u << pg)))
            continue;
        if (!s->never && used && used >= budget)
            return 0;
        b[0] = 0x80; b[1] = (uint8_t)(0xB0u | pg);
        b[2] = 0x00; b[3] = 0x10;           /* column 0 */
        hal_i2c_write(c->st7567.bus, c->st7567.addr, b, 4);
        b[0] = 0x40;
        for (uint32_t x = 0; x < w; x++)
            b[1 + x] = sent[pg * w + x] = fb[pg * w + x];
        hal_i2c_write(c->st7567.bus, c->st7567.addr, b, 1 + w);
        s->pages = (uint8_t)(s->pages & ~(1u << pg));
        used += (5u + w) * ST_COST;
    }
    s->never = 0;
    return budget > used ? budget - used : 0u;
}

const gm_screen_ops_t gm_st7567_ops = { .send = st7567_send };
