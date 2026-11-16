/*
 * tca9555.c: the tca9555 hub type, a TCA9555 16-pin I/O expander on the
 * I2C bus (an external component: core/README.md "External components").
 * Its config is the I2C expanders' .i2c and the hub base's gm_hub_i2c_*
 * keep its pins' directions. It has no pull-ups.
 *
 * SPDX-License-Identifier: MIT
 */
#include "gm_device_config.h"
#include "gm_hal.h"
#include "gm_hub.h"

#define TCA_INPUT  0x00
#define TCA_OUTPUT 0x02
#define TCA_CONFIG 0x06             /* 1 = input */

static void tca_write16(const gm_hub_i2c_t *c, uint8_t reg, uint32_t v)
{
    uint8_t b[3] = { reg, (uint8_t)v, (uint8_t)(v >> 8) };
    hal_i2c_write(c->bus, c->addr, b, 3);
}

static void tca9555_refresh(const gm_hub_t *h, gm_hub_state_t *s)
{
    const gm_hub_i2c_t *c = &h->i2c;
    uint8_t b[2], reg = TCA_INPUT;
    if (s->u.io.in_used && hal_i2c_write(c->bus, c->addr, &reg, 1) == 0 && hal_i2c_read(c->bus, c->addr, b, 2) == 0)
        s->in = b[0] | (uint32_t)b[1] << 8;
}

static void tca9555_flush(const gm_hub_t *h, gm_hub_state_t *s)
{
    if (s->u.io.cfg_dirty) {
        tca_write16(&h->i2c, TCA_CONFIG, ~s->u.io.dir_out & 0xFFFFu);
        s->u.io.cfg_dirty = 0;
        s->never = 1;
    }
    if (s->out != s->written || s->never) {
        tca_write16(&h->i2c, TCA_OUTPUT, s->out);
        s->written = s->out;
        s->never = 0;
    }
}

const gm_hub_ops_t gm_tca9555_ops = {
    .flags = GM_HUBF_BUS | GM_HUBF_IN_REG, .init = gm_hub_i2c_init, .input = gm_hub_i2c_input,
    .output = gm_hub_i2c_output, .refresh = tca9555_refresh, .flush = tca9555_flush,
};
